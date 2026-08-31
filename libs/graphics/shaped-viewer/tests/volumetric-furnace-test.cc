#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/container/array.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The furnace test for the INTEGRATOR, which is the half `openpbr-bsdf-test` cannot reach.
//
// That one measures the closure directly, so nothing it asserts says anything about the walk through a medium: the distance
// sampling, the phase function and the channel-averaged weighting all live in the raygen and are reached only by tracing.
//
// The statement here is the same one, moved up a level.
// Under a UNIFORM environment, a lossless object is invisible — whatever it reflects, refracts or scatters, every path
// eventually leaves and carries the one radiance the environment has everywhere.
// So the image must come back flat, at exactly the environment's value, and any energy the walk loses or invents shows up as
// a deviation from it.
//
// The cases are what separate the interface from the walk, because a flat image alone does not prove the medium was ever
// entered:
//
//   - a clear index-matched interior, which isolates the interface from the walk,
//   - a clear GLASS interior at index 1.5, the only place total internal reflection is measured against a known answer
//     rather than against a bound,
//   - a purely scattering interior, which is the walk itself and must still return the environment,
//   - the same walk at optical depth about 36, which is a walk long enough to have been ended by a budget,
//   - an absorbing interior, which must come back DARKER — the control that proves the medium is not simply ignored.

namespace
{
using namespace cc::primitive_defines;

/// A closed axis-aligned cube as a raw triangle list, wound so every face points outward.
///
/// Closed is the requirement rather than the shape: a path inside an absorbing or scattering interior that escapes to the
/// environment is dropped by the integrator, since it travelled an unbounded distance through the medium.
/// An open shell would lose those paths and read as a loss the medium did not cause.
cc::vector<tg::pos3f> closed_cube(float half_extent)
{
    float const h = half_extent;

    auto const corners = cc::array<tg::pos3f>{
        tg::pos3f(-h, -h, -h), tg::pos3f(+h, -h, -h), tg::pos3f(+h, +h, -h), tg::pos3f(-h, +h, -h),
        tg::pos3f(-h, -h, +h), tg::pos3f(+h, -h, +h), tg::pos3f(+h, +h, +h), tg::pos3f(-h, +h, +h),
    };

    // Each face as two triangles, counter-clockwise seen from outside.
    auto const faces = cc::array<u32>{
        0, 2, 1, 0, 3, 2, // -z
        4, 5, 6, 4, 6, 7, // +z
        0, 1, 5, 0, 5, 4, // -y
        3, 7, 6, 3, 6, 2, // +y
        0, 4, 7, 0, 7, 3, // -x
        1, 2, 6, 1, 6, 5, // +x
    };

    auto out = cc::vector<tg::pos3f>();
    out.reserve(faces.size());
    for (auto const i : faces)
        out.push_back(corners[i]);
    return out;
}

/// One case: what the interior is, and what the image is expected to do about it.
struct furnace_case
{
    cc::string_view name;
    cc::vector<sv::material_attribute_binding> bindings;

    /// Whether the object is lossless, and so whether the image must come back AT the environment rather than below it.
    bool lossless = true;
};

/// The mean of every pixel, plus the extremes — a flat image is what this test is looking for, so the spread matters as
/// much as the centre.
struct image_stats
{
    tg::vec3f mean = tg::vec3f(0, 0, 0);
    float min_luminance = 0.0f;
    float max_luminance = 0.0f;
};

[[nodiscard]] float luminance_of(tg::vec3f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/// Path-traces one case to convergence and reports what came back.
///
/// Accumulated over several dispatches rather than one: the walk through a dense interior is high-variance, and a single
/// frame's spread would swamp the deviation the test is trying to measure.
image_stats trace_furnace(sg::context& ctx,
                          sv::gpu_resource_manager& resources,
                          sv::mesh_data const& mesh,
                          tg::vec3f environment,
                          int frames)
{
    // Small and few, because this runs on a software device: a scattering interior is the most expensive thing the
    // integrator does, and the MEAN — which is what the test actually asserts on — converges long before the pixels do.
    auto const size = tg::vec2i(32, 32);

    auto const item = resources.acquire_scene_item(mesh);
    resources.flush_pending_uploads();
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);

    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    // Close enough that the cube covers the whole frame, so no pixel is answered by the environment alone — every one of
    // them has to go through the object.
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -1.9)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);

    // rgba32_float, which the routine asserts on: the raygen reads the target back to blend into it.
    //
    // PERSISTENT rather than transient, because the accumulation is what this test relies on: the target has to survive the
    // epoch advance between frames, and a transient one is expired by the next.
    auto const target = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                          .width = size[0],
                                                          .height = size[1],
                                                          .usage = sg::texture_usage::readonly_texture
                                                                 | sg::texture_usage::readwrite_texture
                                                                 | sg::texture_usage::copy_src});

    for (auto f = 0; f < frames; ++f)
    {
        auto fc = sv::pt_frame_constants_gpu{};
        fc.camera = sv::camera_gpu::from(cam);

        // No area light at all: a zeroed rect has no normal to face, so the integrator's own intersection rejects it and
        // the environment is the only source.
        // That is what makes "the image equals the environment" the whole statement rather than half of one.
        fc.light = {};

        // Long enough that a path crossing the interface twice and bouncing internally still finishes.
        // Scattering events do not count against this — they have their own cap — so it bounds surface crossings alone.
        //
        // Few samples per dispatch and more dispatches, for the same total.
        // A scattering interior is the heaviest thing the integrator does, and one long dispatch is what trips a driver's
        // watchdog on a slow device — which arrives as a lost device rather than as a slow test.
        fc.samples_per_pixel = 8;
        fc.max_bounces = 12;
        fc.seed = u32(f) + 1u;
        fc.accum_frame = u32(f);

        auto cmd = ctx.create_command_list();

        auto records = cc::vector<sv::instance_gpu>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));

        auto const frame = ctx.transient.create_buffer<sv::pt_frame_constants_gpu>(
            1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
        cmd->upload.pod_to_buffer(frame, fc);

        auto const background = ctx.transient.create_buffer<sv::background_gpu>(
            1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
        cmd->upload.pod_to_buffer(background, sv::background_gpu::from(sv::background::uniform(environment)));

        auto const instance_table = ctx.transient.create_buffer<sv::instance_gpu>(
            records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd->upload.data_to_buffer(instance_table, records);

        auto const bindless = resources.freeze();

        sv::pathtrace_routine::execute(*cmd, {.frame = frame,
                                              .background = background,
                                              .instances = instances,
                                              .output = target,
                                              .instance_table = instance_table,
                                              .hit_groups = hit_groups,
                                              .bindless = &bindless});

        // The routine degrades to a no-op when its shaders do not build, and every number below would then be read off a
        // target nothing ever wrote.
        //
        // Read here but asserted AFTER the list is submitted, which is not a style preference: a `REQUIRE` throws, and
        // unwinding past a recorded-but-unsubmitted command list asserts inside its destructor.
        // A second assertion while the first is unwinding is an immediate `abort`, which loses the message that would have
        // said what went wrong — see the viewer TODO's entry on exactly this.
        auto const ready = sv::pathtrace_routine::is_ready(*cmd);

        // Only the last frame is read: the target holds the running mean of every frame folded in so far, so the
        // intermediate ones say nothing the final one does not.
        auto readback = sg::data_future<tg::vec4f>();
        if (f == frames - 1)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch_and_wait_for_idle();

        REQUIRE(ready);

        if (!readback.is_valid())
            continue;

        // An epoch advance drains the GPU but not the readback actor, so this is the only completion guarantee.
        auto const delivered = ctx.wait_for(readback);
        REQUIRE(delivered.has_value());

        auto const pixels = delivered.value();
        REQUIRE(pixels.size() == isize(size[0]) * isize(size[1]));

        auto stats = image_stats{};
        stats.min_luminance = 1e30f;
        for (auto i = isize(0); i < pixels.size(); ++i)
        {
            auto const c = tg::vec3f(pixels[i][0], pixels[i][1], pixels[i][2]);
            stats.mean += c;
            auto const l = luminance_of(c);
            stats.min_luminance = cc::min(stats.min_luminance, l);
            stats.max_luminance = cc::max(stats.max_luminance, l);
        }
        stats.mean = stats.mean / float(pixels.size());
        return stats;
    }

    FAIL("the furnace trace produced no readback");
    return {};
}
} // namespace

// On the main thread for the same reason every other tracing test is: the shader compiles are driven inline through
// `try_async_blocking_get`, which does not complete from inside a pool worker.
TEST("sv - a lossless interior is invisible under a uniform environment", nx::config::main_thread)
{
    // KNOWN BROKEN on Windows on ARM, and skipped rather than worked around — see the viewer TODO for the evidence.
    //
    // The binary dies through `__fastfail` inside `ctx.advance_epoch_and_wait_for_idle()`, after a trivial dispatch whose
    // command list also recorded an inline readback.
    // Not an assertion and not a lost device: both were instrumented and neither fires, and a fastfail bypasses the SEH
    // filter and the SIGABRT handler nexus installs — which is why it arrived as an exit code with no output at all.
    // These two are the only tests in sv that use `cmd.download`, and sv's own ../docs/structure.md already noted that path had
    // never been exercised here.
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the path-tracing shaders");

    auto const environment = tg::vec3f(0.5f, 0.5f, 0.5f);

    // An index-matched interface, so the surface neither bends nor reflects what crosses it.
    //
    // That is what isolates the MEDIUM: a real index puts a Fresnel and a critical angle between the environment and the
    // interior, and a case built on one measures the two together.
    // The glass case below is the one that measures them together on purpose.
    using binding = sv::material_attribute_binding;

    auto cases = cc::vector<furnace_case>();

    auto const base_bindings = []
    {
        auto out = cc::vector<binding>();
        out.push_back(binding::of("specular_ior", 1.001f));
        out.push_back(binding::of("specular_roughness", 0.05f));
        out.push_back(binding::of("transmission_weight", 1.0f));
        return out;
    };

    cases.push_back({.name = "clear interior", .bindings = base_bindings()});

    // The same clear interior at a REAL index, which is what makes total internal reflection happen.
    //
    // At 1.5 a path bouncing inside meets the critical angle at most of the boundary, and it used to end there — so this
    // case could not exist and the one above index-matched the interface to avoid it.
    // Now that a failed refraction reflects instead, a lossless glass object has to return the environment like any other,
    // and this is the assertion that says so: it is the only place the TIR path is measured against a known answer rather
    // than against a bound.
    auto glass = cc::vector<binding>();
    glass.push_back(binding::of("specular_ior", 1.5f));
    glass.push_back(binding::of("specular_roughness", 0.05f));
    glass.push_back(binding::of("transmission_weight", 1.0f));
    cases.push_back({.name = "glass interior at index 1.5", .bindings = cc::move(glass)});

    // Pure scattering: white transmission color means no absorption at all, so the walk conserves every photon and only
    // moves it around.
    // This is the case the whole test exists for.
    // Optical depth about 3 across the cube, which is thick enough that most paths scatter several times and thin enough
    // that they get out.
    //
    // Kept thin deliberately, so this case measures the estimator on a walk that is easy to get out of.
    // The dense one below is what exercises a walk long enough to have been ended by a budget.
    auto scattering = base_bindings();
    scattering.push_back(binding::of("transmission_depth", 0.25f));
    scattering.push_back(binding::of("transmission_color", tg::vec3f(1, 1, 1)));
    scattering.push_back(binding::of("transmission_scatter", tg::vec3f(0.6f, 0.6f, 0.6f)));
    cases.push_back({.name = "scattering interior", .bindings = cc::move(scattering)});

    // The same walk at optical depth about 36, which is the densest interior this test can afford.
    //
    // An albedo-1 walk escaping a slab of optical depth T takes on the order of T^2 events for the paths that penetrate
    // deepest, so this is the case that exercises a long walk rather than a few turns.
    // It is NOT a regression test for the walk's termination: it passes against a 256-event cap as well, because the paths
    // a cap truncates are a thin enough tail that the loss stays inside this test's 6% margin.
    // What it does cover is a density nothing else here reaches, and it is affordable only because roulette ends the walk —
    // it costs about a third less than the same case against an uncapped budget.
    auto dense = base_bindings();
    dense.push_back(binding::of("transmission_depth", 0.02f));
    dense.push_back(binding::of("transmission_color", tg::vec3f(1, 1, 1)));
    dense.push_back(binding::of("transmission_scatter", tg::vec3f(0.6f, 0.6f, 0.6f)));
    cases.push_back({.name = "dense scattering interior", .bindings = cc::move(dense)});

    // The same walk with absorption, which must come back darker — the control that separates "the medium is lossless"
    // from "the medium was never entered".
    auto absorbing = base_bindings();
    absorbing.push_back(binding::of("transmission_depth", 0.25f));
    absorbing.push_back(binding::of("transmission_color", tg::vec3f(0.5f, 0.5f, 0.5f)));
    absorbing.push_back(binding::of("transmission_scatter", tg::vec3f(0.4f, 0.4f, 0.4f)));
    cases.push_back({.name = "absorbing interior", .bindings = cc::move(absorbing), .lossless = false});

    auto& lib = *sv::acquire_material_library().value();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const positions = closed_cube(0.6f);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto lossless_means = cc::vector<float>();

    for (auto const& c : cases)
    {
        auto const id = lib.acquire(sv::material::create(cc::string(c.name), type, c.bindings));
        auto const mesh = sv::mesh_data{.name = cc::string(c.name),
                                        .geometry = sv::triangle_geometry::create_from_positions(positions),
                                        .material = id};

        auto const stats = trace_furnace(ctx, resources, mesh, environment, 12);
        auto const mean = luminance_of(stats.mean);
        auto const expected = luminance_of(environment);

        if (c.lossless)
        {
            // The whole statement: a lossless object under a uniform environment returns the environment.
            //
            // The margin covers the interface's own single-scattering GGX loss plus what is left of the Monte-Carlo
            // spread at this budget; it does not cover a walk that loses or invents energy, which is a fixed offset and
            // shows up well outside it.
            CHECK(tg::abs(mean - expected) <= 0.06f * expected).context(c.name).dump("mean luminance", mean);

            // Flat, not merely correct on average: a walk that lost energy only where the object is thickest would still
            // average out, and the spread is what catches it.
            // Wide on purpose — at this budget the per-pixel spread is dominated by Monte-Carlo noise, so these bound a
            // catastrophe rather than a convergence.
            CHECK(stats.max_luminance <= expected * 1.6f).context(c.name).dump("max luminance", stats.max_luminance);
            CHECK(stats.min_luminance >= expected * 0.5f).context(c.name).dump("min luminance", stats.min_luminance);

            lossless_means.push_back(mean);
        }
        else
        {
            // Absorption has to actually darken, or the medium is being skipped and every check above passes vacuously.
            CHECK(mean < expected * 0.9f).context(c.name).dump("mean luminance", mean);
            CHECK(mean > 0.0f).context(c.name).dump("mean luminance", mean);
        }
    }

    // The clear interior and the scattering one must agree with each other as well as with the environment: they differ
    // only in the walk, so a difference between them IS the walk's error, with the interface divided out.
    REQUIRE(lossless_means.size() == 4);
    for (auto i = isize(1); i < lossless_means.size(); ++i)
        CHECK(tg::abs(lossless_means[0] - lossless_means[i]) <= 0.05f * luminance_of(environment))
            .dump("clear", lossless_means[0])
            .dump("scattering", lossless_means[i]);
}
