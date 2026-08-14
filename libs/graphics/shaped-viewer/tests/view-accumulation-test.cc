#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Headless, one view traced repeatedly: what a view's persistent record does across frames.
// No pixel readback — the observable surface is the target's identity and the accumulation counter, which is exactly
// what decides whether the shader overwrites the image or blends into it.
//
// Each section uses its own view_id: sections share the enclosing setup rather than re-running it, so a shared id would
// carry one section's accumulation into the next.
TEST("sv - a view accumulates across frames under its id")
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

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::scene_resources::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));

    auto const view_named = [&](char const* name)
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string(name);
        v.resolution = tg::vec2i(64, 64);
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
        return v;
    };

    // One whole frame: both per-frame reclaims, then the trace.
    auto const trace = [&](sv::view_data const& view)
    {
        auto cmd = ctx.create_command_list();
        resources.begin_frame(ctx.current_epoch());
        sv::view_renderer::begin_frame(*cmd);
        auto const target = sv::view_renderer::execute(*cmd, view, resources);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch_and_wait_for_idle();
        return target;
    };
    auto const accumulated = [&](sv::view_id id)
    {
        auto cmd = ctx.create_command_list();
        auto const n = sv::view_renderer::accumulated_frames(*cmd, id);
        ctx.drop_command_list(cc::move(cmd));
        return n;
    };

    SECTION("an unchanged view keeps one target and keeps counting")
    {
        auto const v = view_named("steady");
        auto const first = trace(v);
        REQUIRE(first.raw() != nullptr);
        CHECK(first.width() == 64);
        CHECK(accumulated(v.id) == 1);

        // The same texture across frames is the whole point: a transient one would have nothing to blend into.
        for (auto n = u32(2); n <= 4; ++n)
        {
            auto const again = trace(v);
            CHECK(again.raw().get() == first.raw().get());
            CHECK(accumulated(v.id) == n);
        }
    }

    SECTION("moving the view within the frame does not discard its image")
    {
        auto const v = view_named("moved");
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        // Where a view lands is no longer expressible here at all: a view_data is only the definition of a texture,
        // and the leaf referencing it owns the placement.
        // So relayout cannot reach the trace even by accident, and re-submitting the same view keeps accumulating.
        auto const& resubmitted = v;
        (void)trace(resubmitted);
        CHECK(accumulated(v.id) == 3);
    }

    SECTION("a changed camera restarts the accumulation on the same target")
    {
        auto const v = view_named("orbited");
        auto const first = trace(v);
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 3);

        auto orbited = v;
        orbited.camera.position = tg::pos3d(2.6, 1.8, -3.2);
        auto const after = trace(orbited);
        CHECK(after.raw().get() == first.raw().get()); // same extent, so the target itself survives
        CHECK(accumulated(v.id) == 1);                 // but it was rewritten from scratch
    }

    SECTION("a resize takes a new target and restarts")
    {
        auto const v = view_named("resized");
        auto const first = trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        auto bigger = v;
        bigger.resolution = tg::vec2i(96, 64);
        auto const after = trace(bigger);
        CHECK(after.raw().get() != first.raw().get());
        CHECK(after.width() == 96);
        CHECK(after.height() == 64);
        CHECK(accumulated(v.id) == 1);
    }

    SECTION("a resize at constant aspect still restarts")
    {
        // The sharp case: resolution reaches the upload only through the camera's aspect ratio, so 64x64 and 128x128
        // bake identical constants.
        // The trace hash covers the resolution itself for exactly this reason — otherwise correctness would rest
        // entirely on the resize check, and a same-size texture from a pool would blend two views.
        auto const v = view_named("rescaled");
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        auto scaled = v;
        scaled.resolution = tg::vec2i(128, 128);
        (void)trace(scaled);
        CHECK(accumulated(v.id) == 1);
    }

    SECTION("changed render settings restart the accumulation")
    {
        auto const v = view_named("settings");
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        auto deeper = v;
        sv::ensure_scene_3d(deeper).settings.max_bounces += 1;
        (void)trace(deeper);
        CHECK(accumulated(v.id) == 1);
    }
}
