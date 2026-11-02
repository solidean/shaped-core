#include "wavefront_test_support.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <nexus/async-test.hh>
#include <typed-geometry/scalar/half_float.hh>

// A blue partial coverage in front of the Cornell box's back wall, and the guides each tracer writes behind it.
// A sample's camera ray takes or skips a partial coverage at random, so guides of its hit would flicker with the cutout,
// and a temporal denoiser keeps what its guides explain: the noise.
// So the guides come from guide rays that only a full coverage stops, and behind the ghost they describe the wall, never the ghost.
// The wavefront traces one sample a pixel, as a moving frame does, so a guide of the samples' own hits would show the ghost in about a third of the pixels.

namespace
{
using namespace cc::primitive_defines;
using namespace sv_test::wavefront;

auto constexpr ghost_color = tg::vec3f(0.05f, 0.05f, 1.0f);

/// The ghost spans the image's middle: pixels [12, 20) on both axes lie within it, from the camera of `make_scene`.
auto constexpr ghost_lo = 12;
auto constexpr ghost_hi = 20;

/// A blue quad inside the box, in front of its back wall, covering 0.3 of what it spans.
/// Tilted back to face both the camera and the lamp above it, which a quad standing upright would only graze.
[[nodiscard]] sv::mesh ghost_quad()
{
    auto const lo = -0.3f; // the z of its bottom edge, the nearer one
    auto const hi = -0.7f; // the z of its top edge
    auto positions = cc::vector<tg::pos3f>();
    for (auto const& p : {tg::pos3f(-0.4f, -0.4f, lo), tg::pos3f(0.4f, -0.4f, lo), tg::pos3f(0.4f, 0.4f, hi),
                          tg::pos3f(-0.4f, -0.4f, lo), tg::pos3f(0.4f, 0.4f, hi), tg::pos3f(-0.4f, 0.4f, hi)})
        positions.push_back(p);
    using binding = sv::material_attribute_binding;
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const material = lib.acquire(sv::material::create(
        cc::string("guide ghost"), type, {binding::of("base_color", ghost_color), binding::of("opacity", 0.3f)}));
    return {.name = cc::string("guide ghost"),
            .geometry = sv::triangle_geometry::create_from_positions(positions),
            .material = material};
}

enum class tracer
{
    wavefront,
    megakernel,
};

/// What one frame wrote: the albedo guide where it wrote one, the image, and for the wavefront each pixel's sample count.
struct guide_frame
{
    cc::vector<tg::vec3f> albedo;
    cc::vector<tg::vec3f> image;
    cc::vector<f32> samples;
};

[[nodiscard]] sg::texture_2d guide_target(sg::context& ctx, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d(
        {.format = format,
         .width = k_size[0],
         .height = k_size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
}

/// Traces one frame of `scene` and reads it back, with guide rays where `guided` holds.
/// Unguided, the megakernel still writes guides, of its samples' own hits as before guide rays, and the wavefront writes none.
cc::shared_async<guide_frame> trace_guides(sg::context& ctx,
                                           sv::gpu_resource_manager& resources,
                                           cornell_scene const& scene,
                                           sv::scene_item const& ghost,
                                           tracer t,
                                           bool guided)
{
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto const normal = guide_target(ctx, sg::pixel_format::rgba16_float);
    auto const depth = guide_target(ctx, sg::pixel_format::r32_float);
    auto const albedo = guide_target(ctx, sg::pixel_format::rgba16_float);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    auto const slots = isize(k_size[0]) * isize(k_size[1]);
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, scene.item.mesh, scene.item.instance));
        sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
        records.push_back(resources.describe_instance(*cmd, ghost.mesh, ghost.instance));
        sv::place_instance_record(records.back(), scene.instances[1], scene.hit_groups[1]->opacity);

        // The wavefront's drained frame takes one sample a pixel whatever this says; the megakernel's takes them all,
        // and its guides are their mean, which a guide of the samples' own hits would mix a third of the ghost into.
        auto fc = frame_of(scene, 11u, 0u, t == tracer::megakernel ? 64 : 1);
        // The wavefront routine takes guide rays wherever guides are written and a hit group can cut out;
        // the view decides the same for the megakernel, which reads the flag it sets.
        auto const writes_guides = guided || t == tracer::megakernel;
        fc.write_guides = writes_guides ? 1 : 0;
        fc.guide_frame = 0;
        if (t == tracer::megakernel && guided)
            fc.wavefront_flags |= sv::impl::wavefront_flag_guide_rays;

        auto const bindless = resources.freeze();
        auto const trace = sv::pt_trace_desc{
            .frame = ctx.transient.create_buffer_from_pod(*cmd, fc, sg::buffer_usage::readonly_buffer),
            .background = ctx.transient.create_buffer_from_pod(*cmd, background, sg::buffer_usage::readonly_buffer),
            .instances = scene.instances,
            .output = target,
            .guide_normal = writes_guides ? normal : sg::texture_2d(),
            .guide_depth = writes_guides ? depth : sg::texture_2d(),
            .guide_albedo = writes_guides ? albedo : sg::texture_2d(),
            .instance_table = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer),
            .lights = sv_test::upload_lights(*cmd, scene.lights),
            .hit_groups = scene.hit_groups,
            .bindless = &bindless};

        auto drained = sg::data_future<u32>();
        auto const outcome = t == tracer::wavefront
                               ? sv::wavefront_routine::execute(*cmd, {.trace = trace,
                                                                       .frame = fc,
                                                                       .state = &state,
                                                                       .rounds = drain_rounds_of(k_max_bounces),
                                                                       .restart = true,
                                                                       .regenerate = false,
                                                                       .live_paths = &drained})
                               : sv::pathtrace_routine::execute(*cmd, trace);
        auto const ready = outcome == sg::routine_outcome::executed;
        auto albedo_back = sg::data_future<u16>();
        auto image_back = sg::data_future<tg::vec4f>();
        auto tallies = sg::data_future<sv::shaders::tracer::pixel_tally>();
        if (ready)
        {
            if (writes_guides)
                albedo_back = sg::data_future<u16>(cmd->download.bytes_from_texture(albedo.raw()));
            image_back = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));
            if (t == tracer::wavefront)
                tallies = cmd->download.data_from_buffer(state.tallies);
        }
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (!ready)
        {
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            sv_test::drive_ambient_work();
            continue;
        }

        // Every sample the frame started finished in it, the guide ray's round included.
        if (t == tracer::wavefront)
            check_drained(drained, "the guided frame");

        auto out = guide_frame{};
        auto const img = image_back.try_get_data();
        REQUIRE(img.has_value());
        REQUIRE(img.value().size() == slots);
        for (auto i = isize(0); i < slots; ++i)
            out.image.push_back(tg::vec3f(img.value()[i][0], img.value()[i][1], img.value()[i][2]));
        if (writes_guides)
        {
            auto const a = albedo_back.try_get_data();
            REQUIRE(a.has_value());
            REQUIRE(a.value().size() == 4 * slots);
            auto const half = [&](isize i) { return tg::f16::make_from_bits(a.value()[i]).to_f32(); };
            for (auto i = isize(0); i < slots; ++i)
                out.albedo.push_back(tg::vec3f(half(4 * i), half(4 * i + 1), half(4 * i + 2)));
        }
        if (t == tracer::wavefront)
        {
            auto const tl = tallies.try_get_data();
            REQUIRE(tl.has_value());
            for (auto i = isize(0); i < slots; ++i)
                out.samples.push_back(tl.value()[i].count);
        }
        co_return out;
    }
}

/// How blue a color leans: its blue less its red, 0 for the box's white wall and about 0.95 for the ghost.
[[nodiscard]] f32 blueness(tg::vec3f c)
{
    return c[2] - c[0];
}
} // namespace

ASYNC_INVOCABLE_TEST("sv tracers - behind a partial coverage the guides describe the surface every sample sees, never "
                     "the coverage",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = make_scene(resources);
    auto const ghost = resources.acquire_scene_item(ghost_quad());
    resources.wait_for_pending_uploads();
    auto const* const permutation = resources.shaders.find(ghost.shader_key);
    REQUIRE(permutation != nullptr);
    REQUIRE(permutation->opacity.can_cut_out());
    scene.instances.push_back(sg::tlas_instance{.blas = sv_test::blas_of(resources, ghost.mesh),
                                                .instance_id = 1,
                                                .hit_group_offset = 2,
                                                .opaque_override = false});
    scene.hit_groups.push_back(permutation);

    for (auto const t : {tracer::wavefront, tracer::megakernel})
    {
        auto const name = t == tracer::wavefront ? "wavefront" : "megakernel";
        auto const guided = co_await trace_guides(ctx, resources, scene, ghost, t, true);
        auto const unguided = co_await trace_guides(ctx, resources, scene, ghost, t, false);

        // How many of the guides behind the ghost lean blue.
        auto const blue_guides_of = [](guide_frame const& f)
        {
            auto count = 0;
            for (auto y = ghost_lo; y < ghost_hi; ++y)
                for (auto x = ghost_lo; x < ghost_hi; ++x)
                    if (blueness(f.albedo[isize(y) * k_size[0] + x]) > 0.1f)
                        ++count;
            return count;
        };
        auto const ghost_pixels = (ghost_hi - ghost_lo) * (ghost_hi - ghost_lo);

        // Behind the ghost every guide is the wall's.
        auto const blue = blue_guides_of(guided);
        CHECK(blue == 0).context(cc::format("{}: {} of {} guides behind the ghost lean blue", name, blue, ghost_pixels));

        // The samples do reach the ghost: guides of their own hits, as the megakernel wrote them before guide rays, lean blue.
        // Measured at 64 of 64, so the scene tests what it means to.
        if (t == tracer::megakernel)
        {
            auto const old_blue = blue_guides_of(unguided);
            CHECK(old_blue > ghost_pixels / 2)
                .context(cc::format("{}: only {} of {} guides of the samples' own hits lean blue", name, old_blue,
                                    ghost_pixels));
        }

        // The guide ray leaves the samples' streams where they were, so the image is the same with and without it, bit for bit.
        auto differing = 0;
        for (auto i = isize(0); i < guided.image.size(); ++i)
            if (guided.image[i] != unguided.image[i])
                ++differing;
        CHECK(differing == 0).context(cc::format("{}: {} pixels differ with guide rays", name, differing));

        // A guide ray is no sample, so a frame of one sample a pixel holds exactly one.
        for (auto i = isize(0); i < guided.samples.size(); ++i)
            CHECK(guided.samples[i] == 1.0f)
                .context(cc::format("{}: pixel {} holds {} samples", name, i, guided.samples[i]));
    }
}
