#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

// Headless end-to-end through the view_renderer routine: it path-traces one view into a transient target and
// blits it into an offscreen render target, all on one command list. Beyond the per-routine tests, this
// exercises the orchestration and — the part a real frame relies on — the trace -> blit transition on a single
// command list, which the WARP debug layer validates for us.
//
// No pixel readback: reaching the end without an assert / exception / debug-layer error means the whole frame
// (trace + barrier + blit) recorded and ran.
TEST("sv - view renderer end to end (headless)")
{
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

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    // Build the scene through the managers (this is where the BLAS is built).
    auto const cloud = sv_test::make_triangle_cloud(64);
    auto resources = sv::scene_resources::create(ctx);
    auto const mesh = resources.meshes.acquire(cloud.positions);
    auto const materials = resources.materials.acquire(cloud.materials);
    REQUIRE(resources.meshes.contains(mesh));
    REQUIRE(resources.materials.contains(materials));

    auto const size = tg::vec2i(128, 128);

    auto def = sv::viewer_definition{};
    {
        auto v = sv::view{};
        v.id = sv::view_id::from_string("headless");
        v.size = size;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)}; // default orientation frames the origin
        v.items.push_back({.mesh = mesh, .materials = materials});
        // Lights are a typed list on the view — the default rect (overhead, facing down) with a brighter emission.
        // Exercises the area_light rectangle+transform -> world-rect derivation the view_renderer does.
        v.area_lights.push_back({.emission = tg::vec3f(18.0f, 18.0f, 18.0f)});
        def.views.push_back(cc::move(v));
    }

    // The output the renderer blits into — a plain offscreen color target here; a real frame passes a backbuffer.
    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = size[0],
                                                          .height = size[1],
                                                          .usage = sg::texture_usage::render_target});

    auto cmd = ctx.create_command_list();
    sv::view_renderer::execute(*cmd, def, resources, output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    CHECK(true); // trace + blit recorded and ran on one command list without a device / barrier error
}
