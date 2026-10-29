#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/thorough.hh> // nx::is_thorough
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// Cutout opacity is read through each instance's record rather than its material's code, one reader for every material.
// So each way an opacity can be sourced — a constant, a mesh attribute, a texture channel, thresholded or not — is traced here and read back.
//
// A black, unlit quad fills the view in front of a white environment.
// A pixel is then exactly 0 where the quad was kept and exactly the environment where it was cut, whatever else the tracer does.

namespace
{
using namespace cc::primitive_defines;

/// What one trace came back as: how many pixels were kept, how many cut, and how many neither.
struct cut_counts
{
    isize kept = 0;
    isize cut = 0;
    isize other = 0;
};

/// A quad at z = 0, from -2 to 2 on x and y, as two triangles split along its diagonal.
/// The camera sees x and y within about 1.4 of the origin, so the quad covers every pixel and each triangle about half of them.
[[nodiscard]] cc::vector<tg::pos3f> quad_positions()
{
    auto out = cc::vector<tg::pos3f>();
    out.push_back(tg::pos3f(-2, -2, 0));
    out.push_back(tg::pos3f(2, -2, 0));
    out.push_back(tg::pos3f(2, 2, 0));
    out.push_back(tg::pos3f(-2, -2, 0));
    out.push_back(tg::pos3f(2, 2, 0));
    out.push_back(tg::pos3f(-2, 2, 0));
    return out;
}

/// A quad of `type`, black and unlit, opaque unless the bindings or `attributes` give it an opacity.
[[nodiscard]] sv::mesh quad_of(cc::string_view name,
                               cc::string_view type_name,
                               cc::vector<sv::material_attribute_binding> bindings,
                               cc::vector<sv::mesh_attribute> attributes)
{
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(type_name).value();
    auto const material = lib.acquire(sv::material::create(cc::string(name), type, cc::move(bindings)));
    return {.name = cc::string(name),
            .geometry = sv::triangle_geometry::create_from_positions(quad_positions()),
            .attributes = cc::move(attributes),
            .material = material};
}

/// Traces `mesh` once and sorts its pixels into kept and cut.
cc::shared_async<cut_counts> trace_cut(sg::context& ctx, sv::gpu_resource_manager& resources, sv::mesh const& mesh)
{
    auto const item = resources.acquire_scene_item(mesh);
    resources.wait_for_pending_uploads();
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);
    REQUIRE(permutation->opacity.can_cut_out());

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh),
                                          .instance_id = 0,
                                          .hit_group_offset = 0,
                                          .opaque_override = !permutation->opacity.can_cut_out()});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    auto const size = tg::vec2i(32, 32);
    auto const environment = tg::vec3f(1, 1, 1);
    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();

        auto fc = sv::default_frame_constants();
        fc.camera = sv::camera_record_of(cam);
        fc.samples_per_pixel = 1;
        fc.max_bounces = 1;

        auto cmd = ctx.create_command_list();
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), instances[0], permutation->opacity);

        auto const frame = ctx.transient.create_buffer_from_pod(*cmd, fc, sg::buffer_usage::readonly_buffer);
        auto const background = ctx.transient.create_buffer_from_pod(
            *cmd, sv::background_gpu::from(sv::background::uniform(environment)), sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const bindless = resources.freeze();

        auto const traced = sv::pathtrace_routine::execute(*cmd, {.frame = frame,
                                                                  .background = background,
                                                                  .instances = instances,
                                                                  .output = target,
                                                                  .instance_table = instance_table,
                                                                  .hit_groups = hit_groups,
                                                                  .bindless = &bindless});
        auto const ready = traced == sg::routine_outcome::executed;
        auto readback = sg::data_future<tg::vec4f>();
        if (ready)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (!ready)
        {
            REQUIRE(cc::current_time_steady_secs() - start < 45.0);
            sv_test::drive_ambient_work();
            continue;
        }

        auto const pixels = readback.try_get_data();
        REQUIRE(pixels.has_value());
        auto counts = cut_counts{};
        for (auto const& p : pixels.value())
        {
            auto const l = (p[0] + p[1] + p[2]) / 3.0f;
            if (tg::abs(l) < 0.02f)
                ++counts.kept;
            else if (tg::abs(l - 1.0f) < 0.02f)
                ++counts.cut;
            else
                ++counts.other;
        }
        co_return counts;
    }
}

/// The two triangles' values, per triangle.
[[nodiscard]] sv::mesh_attribute per_triangle(cc::string_view name, f32 first, f32 second)
{
    return sv::mesh_attribute::create(cc::string(name), sv::attribute_frequency::per_triangle,
                                      cc::array<f32>{first, second});
}

/// The cases each expect: everything cut, half of it, or nothing.
enum class expect_cut
{
    all,
    half,
    none,
};

void check_counts(cut_counts const& c, expect_cut expected, cc::string_view name)
{
    auto const total = c.kept + c.cut + c.other;
    CHECK(c.other == 0).context(cc::format("{}: {} pixels neither kept nor cut", name, c.other));
    switch (expected)
    {
    case expect_cut::all:
        CHECK(c.cut == total).context(cc::format("{}: {} of {} cut", name, c.cut, total));
        break;
    case expect_cut::none:
        CHECK(c.kept == total).context(cc::format("{}: {} of {} kept", name, c.kept, total));
        break;
    case expect_cut::half:
        // The diagonal runs through pixel centers, so the halves differ by its own pixels at most.
        CHECK(c.cut > total * 4 / 10).context(cc::format("{}: {} of {} cut", name, c.cut, total));
        CHECK(c.kept > total * 4 / 10).context(cc::format("{}: {} of {} kept", name, c.kept, total));
        break;
    }
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::pathtrace_routine - a cutout cuts where its opacity says, however the opacity is sourced",
                     (sg::context_handle const& ctx_h))
{
    // The inline readback path, which volumetric-furnace-test documents as fastfailing on Windows on ARM.
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

    using binding = sv::material_attribute_binding;
    auto const unlit = sv::builtin_material::unlit;
    auto const openpbr = sv::builtin_material::openpbr;
    auto const black = binding::of("color", tg::vec3f(0, 0, 0));

    // openpbr with nothing that reflects, transmits or emits, so a kept pixel is black too.
    auto const dark_openpbr = [](f32 cutoff)
    {
        auto out = cc::vector<binding>();
        out.push_back(binding::of("base_weight", 0.0f));
        out.push_back(binding::of("specular_weight", 0.0f));
        out.push_back(binding::of("alpha_cutoff", cutoff));
        return out;
    };

    auto resources = sv::gpu_resource_manager::create(ctx);

    // A default run traces a thresholded attribute both ways and a texture channel; --thorough traces every source.
    // A half cut counts pixels, not which half, so only the case cutting nothing tells a reversed cutoff apart.
    {
        auto attributes = cc::vector<sv::mesh_attribute>();
        attributes.push_back(per_triangle("opacity", 0.3f, 0.7f));
        auto const c = co_await trace_cut(
            ctx, resources, quad_of("cutout threshold", openpbr, dark_openpbr(0.5f), cc::move(attributes)));
        check_counts(c, expect_cut::half, "a cutoff of 0.5 over opacities of 0.3 and 0.7");
    }
    {
        auto attributes = cc::vector<sv::mesh_attribute>();
        attributes.push_back(per_triangle("opacity", 0.6f, 0.9f));
        auto const c = co_await trace_cut(
            ctx, resources, quad_of("cutout threshold kept", openpbr, dark_openpbr(0.5f), cc::move(attributes)));
        check_counts(c, expect_cut::none, "a cutoff of 0.5 over opacities of 0.6 and 0.9");
    }
    if (nx::is_thorough())
    {
        {
            auto bindings = cc::vector<binding>{black, binding::of("opacity", 0.0f)};
            auto const c = co_await trace_cut(ctx, resources, quad_of("cutout constant", unlit, cc::move(bindings), {}));
            check_counts(c, expect_cut::all, "a constant opacity of 0");
        }
        {
            auto attributes = cc::vector<sv::mesh_attribute>();
            attributes.push_back(per_triangle("opacity", 0.0f, 1.0f));
            auto const c = co_await trace_cut(
                ctx, resources, quad_of("cutout attribute", unlit, cc::vector<binding>{black}, cc::move(attributes)));
            check_counts(c, expect_cut::half, "a per-triangle opacity of 0 and 1");
        }
    }
    {
        // A 2x1 texture whose left texel is transparent and right one opaque, read through its alpha channel.
        // u runs with x across the quad, so the left half of the view is cut.
        auto uvs = cc::array<tg::vec2f>::create_defaulted(6);
        auto const positions = quad_positions();
        for (auto i = isize(0); i < uvs.size(); ++i)
            uvs[i] = tg::vec2f((positions[i][0] + 2.0f) / 4.0f, 0.5f);
        auto attributes = cc::vector<sv::mesh_attribute>();
        attributes.push_back(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, cc::move(uvs)));
        auto mesh = quad_of("cutout texture", unlit, cc::vector<binding>{black}, cc::move(attributes));

        auto pixels = cc::array<byte>::create_filled(2 * 4, byte(0));
        pixels[7] = byte(255);
        mesh.textures.push_back(
            {.name = "opacity",
             .source = {.texture = sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 2, 1),
                        .uv_attribute = "uv",
                        .sampler = {.mag_filter = sg::sampler_filter::nearest,
                                    .address_u = sg::sampler_address_mode::clamp_edge,
                                    .address_v = sg::sampler_address_mode::clamp_edge},
                        .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::a)}});
        auto const c = co_await trace_cut(ctx, resources, mesh);
        check_counts(c, expect_cut::half, "a texture's alpha channel");
    }

    co_await cc::async_settled(sv::background_work(ctx));
}
