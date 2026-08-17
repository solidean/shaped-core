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

    // The persistent half is the caller's here, exactly as it is a viewer's — no window in sight.
    auto store = sv::view_store{};

    // One whole frame: both per-frame reclaims, then the trace.
    auto const trace = [&](sv::view_data const& view)
    {
        auto cmd = ctx.create_command_list();
        resources.begin_frame(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const target = sv::view_renderer::execute(*cmd, view, resources, store);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch_and_wait_for_idle();
        return target;
    };
    auto const accumulated = [&](sv::view_id id) { return store.accumulated_frames(id); };

    SECTION("an unchanged view keeps counting, alternating its ping-pong pair")
    {
        auto const v = view_named("steady");
        auto const first = trace(v);
        REQUIRE(first.raw() != nullptr);
        CHECK(first.width() == 64);
        CHECK(accumulated(v.id) == 1);

        // The pair alternates: a reprojecting read cannot alias its own write, so the texture handed back is the
        // half written *this* frame.
        // It returns to the first one every second frame.
        auto const second = trace(v);
        CHECK(second.raw().get() != first.raw().get());
        CHECK(accumulated(v.id) == 2);

        auto const third = trace(v);
        CHECK(third.raw().get() == first.raw().get());
        CHECK(accumulated(v.id) == 3);
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

    // The point of reprojection, and the assertion that inverted when it landed.
    // A camera move used to throw the converged image away and start from noise.
    // Now the estimator is per pixel and the history is reprojected, so the view keeps counting through the move.
    SECTION("a changed camera keeps the accumulation rather than restarting it")
    {
        auto const v = view_named("orbited");
        (void)trace(v);
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 3);

        auto orbited = v;
        orbited.camera.position = tg::pos3d(2.6, 1.8, -3.2);
        (void)trace(orbited);
        CHECK(accumulated(v.id) == 4); // carried through, not reset

        // And it keeps going while the camera keeps moving — each frame reprojects the last one's image.
        orbited.camera.position = tg::pos3d(2.8, 1.8, -3.2);
        (void)trace(orbited);
        CHECK(accumulated(v.id) == 5);
    }

    // The scene changing is the one thing reprojection cannot rescue: the pixels may still line up, but what they
    // show is a different image, so the whole estimate has to go.
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

    SECTION("a resize takes a new target and restarts")
    {
        auto const v = view_named("resized");
        auto const first = trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        // A resize reallocates both halves of the pair, so there is no history to reproject at all.
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
        // The sharp case: resolution reaches the upload only through the camera's aspect ratio, so 64x64 and
        // 128x128 bake identical constants — and the camera is no longer hashed at all.
        // What catches it is the resize check on the pair itself, which is why the textures are reallocated rather
        // than pooled.
        auto const v = view_named("rescaled");
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        auto scaled = v;
        scaled.resolution = tg::vec2i(128, 128);
        (void)trace(scaled);
        CHECK(accumulated(v.id) == 1);
    }
}

// The same property as above, but down the *plan* path — `viewer_renderer::execute` -> `view_renderer::resolve` +
// `::trace` — which is what sv::viewer actually runs and what every window sees.
//
// It gets its own test because the two paths keep their own bookkeeping: `execute` above resolves the one slot it
// needs itself, while the plan path resolves every view's slots up front and traces them afterwards.
// A regression in one is invisible from the other, and this one is the path that matters.
// Pinned to the main thread, since the routine's shader compiles do not complete from a pool worker and `execute`
// then silently traces nothing — see pathtraced-view-test.
TEST("sv - a view accumulates across frames down the plan path", nx::config::main_thread)
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

    auto const output_size = tg::vec2i(64, 64);
    auto const traced_id = sv::view_id::from_string("planned");

    // One traced view, wrapped in the root layout view sv::viewer synthesizes per frame.
    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = traced_id;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
        def.views.push_back(cc::move(v));

        auto const root_node = def.nodes.add_container(sv::invalid_node);
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(sv::view_index(0));
        def.nodes.add_leaf(root_node, cc::move(leaf));

        auto root = sv::view_data{};
        root.id = sv::view_id::from_string("root");
        root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
        def.root_view = sv::view_index(def.views.size());
        def.views.push_back(cc::move(root));
    }

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});

    auto store = sv::view_store{};

    auto const frame = [&](u64 index)
    {
        auto cmd = ctx.create_command_list();
        resources.begin_frame(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));

        // No history fed in: this asserts the store's own bookkeeping, not the refresh policy's.
        auto const plan = sv::build_render_plan(def, output_size, index, {});
        REQUIRE(sv::pathtrace_routine::is_ready(*cmd)); // a dead shader traces nothing and would pass every check below

        sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                     output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch_and_wait_for_idle();
    };

    // The regression this exists for: `resolve` stamped the *declaration's* reset rule onto the slot, `trace` then
    // compared its own content hash against it, and the mismatch restarted the estimator on every frame.
    // The counter stayed pinned at 1 while every other check in the suite went on passing.
    for (auto i = u64(1); i <= 4; ++i)
    {
        frame(i);
        CHECK(store.accumulated_frames(traced_id) == u32(i));
    }

    // Every slot the trace writes must rotate, not just the one whose counter is read above.
    //
    // The G-buffer has no counter of its own — nothing increments it — so deciding rotation per slot froze its pair:
    // the history half stayed the texture nothing had ever written, every pixel failed the disocclusion test against
    // garbage geometry, and the whole image rejected its history on every frame.
    // The counter above kept climbing throughout, which is exactly why it could not catch this.
    auto const* const rec = store.peek(traced_id);
    REQUIRE(rec != nullptr);

    for (auto const tid : {sv::temporal_id::accumulation(0), sv::temporal_id::gbuffer(0)})
    {
        auto const* const slot = rec->temporal.get_ptr(tid);
        REQUIRE(slot != nullptr);

        CHECK(slot->has_history);                          // it carries a previous frame
        CHECK(slot->texture.raw() != slot->history.raw()); // as a genuine pair, not one texture twice
        CHECK(slot->history.raw() != nullptr);
    }
}
