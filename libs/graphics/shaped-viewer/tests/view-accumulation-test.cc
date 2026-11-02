#include "viewer_test_env.hh"

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Headless, one view traced repeatedly: what a view's persistent record does across frames.
// No pixel readback — the observable surface is the target's identity and the accumulation counter, which is exactly
// what decides whether the shader overwrites the image or blends into it.
ASYNC_INVOCABLE_TEST("sv - a view accumulates across frames under its id", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cloud", cloud.positions, cloud.materials));

    // An acquire queues the upload and the BLAS build; a test tracing what it just built has no frame loop
    // to drain that queue, so it drains it itself.
    resources.wait_for_pending_uploads();

    auto const view_named = [&](char const* name)
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string(name);
        v.resolution = tg::vec2i(64, 64);
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        sv::ensure_scene_3d(v).items.push_back(item);
        return v;
    };

    // The persistent half is the caller's here, exactly as it is a viewer's — no window in sight.
    auto store = sv::view_store{};

    auto const accumulated = [&](sv::view_id id) { return store.accumulated_frames(id); };

    // One whole frame: both per-frame reclaims, then the trace.
    //
    // Driven until the accumulator actually moves, rather than run once.
    // What this test is about is how many TRACED frames a view has folded in, and a frame that declined folded in
    // nothing — so counting it would make the assertions below depend on whether a state object happened to finish,
    // which under a parallel suite it sometimes does not.
    // See sv_test::frames_until_executed, of which this is the view-scoped twin.
    auto const trace = [&](sv::view_data const& view)
    {
        auto const before = accumulated(view.id);
        auto target = sg::texture_2d();
        auto const moved = sv_test::frames_until_executed(
            ctx,
            [&](sg::command_list& cmd)
            {
                resources.advance_to(ctx.current_epoch());
                store.begin_frame(u64(ctx.current_epoch()));
                target = sv::view_renderer::execute(cmd, view, resources, store);
                // CHANGED rather than increased: a trace whose inputs moved restarts the estimate, so the counter
                // drops back to 1 — which is still a frame that dispatched, and is exactly what the camera sections
                // below are about.
                // A declined frame is the only one that leaves it untouched.
                return accumulated(view.id) != before ? sg::routine_outcome::executed : sg::routine_outcome::declined;
            });
        REQUIRE(moved);
        return target;
    };

    // A frame shaded through a stand-in for a permutation still compiling restarts the mean, which `trace` counts as moved.
    // So a throwaway view is traced until two frames accumulate, and every section below starts with the shaders built.
    {
        auto const warm = view_named("warm-up");
        auto const warmed = sv_test::frames_until_executed(
            ctx,
            [&](sg::command_list& cmd)
            {
                resources.advance_to(ctx.current_epoch());
                store.begin_frame(u64(ctx.current_epoch()));
                (void)sv::view_renderer::execute(cmd, warm, resources, store);
                return accumulated(warm.id) >= 2 ? sg::routine_outcome::executed : sg::routine_outcome::declined;
            });
        REQUIRE(warmed);
    }

    SECTION("an unchanged view keeps counting into the one target")
    {
        auto const v = view_named("steady");
        auto const first = trace(v);
        REQUIRE(first.raw() != nullptr);
        CHECK(first.width() == 64);
        CHECK(accumulated(v.id) == 1);

        // One texture, not a pair: the raygen blends at the pixel it is about to write, so the same target comes
        // back every frame and its content is the running mean of everything traced into it.
        auto const second = trace(v);
        CHECK(second.raw().get() == first.raw().get());
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

    // The trade the accumulator makes: every sample it holds was drawn through one eye, so the mean it converges to
    // is the image this frame is asking for and no other.
    // Moving the camera therefore throws the estimate away — and buys, in exchange, an uncapped exact mean with no
    // reprojection heuristic to smear or to reject.
    SECTION("a changed camera restarts the accumulation")
    {
        auto const v = view_named("orbited");
        (void)trace(v);
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 3);

        auto orbited = v;
        orbited.camera.position = tg::pos3d(2.6, 1.8, -3.2);
        (void)trace(orbited);
        CHECK(accumulated(v.id) == 1);

        // Parked at the new eye, it converges again from there.
        (void)trace(orbited);
        CHECK(accumulated(v.id) == 2);

        // And every further move restarts it again, which is what makes an orbit render at the per-frame sample
        // count and settle the moment it stops.
        orbited.camera.position = tg::pos3d(2.8, 1.8, -3.2);
        (void)trace(orbited);
        CHECK(accumulated(v.id) == 1);
    }

    // The other half of the rule: what the trace uploads decides the image just as much as where it is seen from.
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

    // What a capture waits on: a megakernel frame adds `samples_per_pixel` to every pixel, so its count is a product.
    SECTION("a megakernel layer holds its frames' samples")
    {
        auto v = view_named("megakernel samples");
        auto& settings = sv::ensure_scene_3d(v).settings;
        settings.mode = sv::render_mode::megakernel;
        settings.samples_per_pixel = 3;
        (void)trace(v);
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 3);
        CHECK(store.min_accumulated_samples(v.id) == 9);
        CHECK(store.is_accumulation_converged(v.id, 9));
        CHECK(!store.is_accumulation_converged(v.id, 10));

        // A restart counts nothing of what the old image held.
        settings.max_bounces += 1;
        (void)trace(v);
        CHECK(store.min_accumulated_samples(v.id) == 3);
    }

    SECTION("a resize takes a new target and restarts")
    {
        auto const v = view_named("resized");
        auto const first = trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        // A resize reallocates the target, so there is nothing left to blend into.
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
        // 128x128 bake identical constants — the camera is hashed, and hashes the same either way.
        // What catches it is the resize check on the target itself, which is why it is reallocated rather than
        // pooled.
        auto const v = view_named("rescaled");
        (void)trace(v);
        (void)trace(v);
        REQUIRE(accumulated(v.id) == 2);

        auto scaled = v;
        scaled.resolution = tg::vec2i(128, 128);
        (void)trace(scaled);
        CHECK(accumulated(v.id) == 1);
    }

    // Reached at the end of every pass, so what one section's traces detached never outlives it.
    co_await cc::async_settled(sv::background_work(ctx));
}

// The same property as above, but down the *plan* path — `viewer_renderer::execute` -> `view_renderer::resolve` +
// `::trace` — which is what sv::viewer actually runs and what every window sees.
//
// It gets its own test because the two paths keep their own bookkeeping: `execute` above resolves the one slot it
// needs itself, while the plan path resolves every view's slots up front and traces them afterwards.
// A regression in one is invisible from the other, and this one is the path that matters.
ASYNC_INVOCABLE_TEST("sv - a view accumulates across frames down the plan path", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cloud", cloud.positions, cloud.materials));

    // An acquire queues the upload and the BLAS build; a test tracing what it just built has no frame loop
    // to drain that queue, so it drains it itself.
    resources.wait_for_pending_uploads();

    // The warm-up below must trace with the item's own hit group rather than the fallback standing in while it compiles.
    // The frame that compile lands on builds a new pipeline and declines, which the counted frames cannot absorb.
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);
    co_await cc::async_settled(permutation->hit_group);

    auto const output_size = tg::vec2i(64, 64);
    auto const traced_id = sv::view_id::from_string("planned");

    // One traced view, wrapped in the root layout view sv::viewer synthesizes per frame.
    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = traced_id;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        sv::ensure_scene_3d(v).items.push_back(item);
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

    // A coroutine lambda, awaited on the spot, so what it captures by reference outlives every suspend.
    auto const frame = [&](u64 index) -> cc::shared_async<cc::unit>
    {
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));

        // No history fed in: this asserts the store's own bookkeeping, not the refresh policy's.
        auto const plan = sv::build_render_plan(def, output_size, index, {});
        CHECK(sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                           output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)))
              == sg::routine_outcome::executed);

        // A dead shader traces nothing and would pass every check below.
        // The frame's own outcome is what says it did not, and it is checked above rather than read off the routine.
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    };

    // Warmed first, for the same reason as the test above: the counted frames must all be frames that dispatched.
    // Frame index 0 is outside the 1..4 the loop below uses, so nothing here is counted twice.
    REQUIRE(sv_test::frames_until_executed(ctx,
                                           [&](sg::command_list& cmd)
                                           {
                                               resources.advance_to(ctx.current_epoch());
                                               store.begin_frame(u64(ctx.current_epoch()));
                                               auto const plan = sv::build_render_plan(def, output_size, 0, {});
                                               return sv::viewer_renderer::execute(
                                                   cmd, def, plan, resources, store,
                                                   output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                                           }));

    // Whichever tracer the default mode resolves to may still compile its own kernel for the item, standing in meanwhile.
    // A pending stand-in restarts the image every frame, so the counted frames start only once a frame binds none.
    auto const stand_in_hash = [&]
    {
        auto const* const rec = store.peek_ptr(traced_id);
        auto const* const slot = rec != nullptr ? rec->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
        return slot != nullptr ? slot->stand_in_hash : u64(1);
    };
    auto const warm_start = cc::current_time_steady_secs();
    while (stand_in_hash() != 0)
    {
        REQUIRE(cc::current_time_steady_secs() - warm_start < 60.0);
        sv_test::drive_ambient_work();
        co_await frame(0);
    }
    store = sv::view_store{}; // the warm-up's own counts are not what this test is about

    // The regression this exists for: `resolve` stamped the *declaration's* reset rule onto the slot, `trace` then
    // compared its own content hash against it, and the mismatch restarted the estimator on every frame.
    // The counter stayed pinned at 1 while every other check in the suite went on passing.
    for (auto i = u64(1); i <= 4; ++i)
    {
        co_await frame(i);
        CHECK(store.accumulated_frames(traced_id) == u32(i));
    }

    // The accumulator is the one texture the estimate lives in, and the primary depth the only resource beside it.
    //
    // A second accumulation slot used to exist for the reprojection's G-buffer, together with a history half each; the
    // blend is in place now, so a resolve that hands back any more than these two is a regression.
    auto const* const rec = store.peek_ptr(traced_id);
    REQUIRE(rec != nullptr);
    CHECK(rec->temporal.size() == 2);
    CHECK(rec->temporal.get_ptr(sv::temporal_id::primary_depth(0)) != nullptr);

    auto const* const slot = rec->temporal.get_ptr(sv::temporal_id::accumulation(0));
    REQUIRE(slot != nullptr);
    CHECK(slot->texture.raw() != nullptr);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A material whose fragment does not compile is drawn through the stand-in for good, which is an image like any other.
// It used to restart its view on every frame: the frame reported a substitution and the next one took that for a changed image.
// So the view never accumulated, and a capture waiting on it timed out.
ASYNC_INVOCABLE_TEST("sv - a material that does not compile still accumulates down the plan path",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    auto& lib = *sv::acquire_material_library().value();
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    auto const type
        = lib.register_type(sv::material_type::create({.name = "sv_test_broken_planned",
                                                       .signature = cc::move(signature),
                                                       .shader = "surface.x = not_a_function(roughness)\n"}));

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto mesh = sv_test::as_mesh("broken cloud", cloud.positions, cloud.materials);
    mesh.material = lib.acquire(sv::material::create("sv_test_broken_planned", type, {}));
    auto const item = resources.acquire_scene_item(mesh);
    resources.wait_for_pending_uploads();

    // Each tracer says once why the cloud shades as the stand-in, however many frames trace it.
    nx::expect_error("the hit group of <material 'sv_test_broken_planned'> did not compile*", nx::exactly(1));
    if (ctx.supports(sg::feature::ray_query))
        nx::expect_error("the shade kernel of <material 'sv_test_broken_planned'> did not compile*", nx::exactly(1));

    auto const output_size = tg::vec2i(64, 64);
    for (auto const mode : {sv::render_mode::megakernel, sv::render_mode::wavefront})
    {
        if (mode == sv::render_mode::wavefront && !ctx.supports(sg::feature::ray_query))
            continue;

        auto const traced_id
            = sv::view_id::from_string(mode == sv::render_mode::wavefront ? "broken wavefront" : "broken megakernel");
        auto def = sv::viewer_definition{};
        {
            auto v = sv::view_data{};
            v.id = traced_id;
            v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
            auto& scene = sv::ensure_scene_3d(v);
            scene.items.push_back(item);
            scene.settings.mode = mode;
            def.views.push_back(cc::move(v));

            auto const root_node = def.nodes.add_container(sv::invalid_node);
            auto leaf = sv::layout_leaf{};
            leaf.views.push_back(sv::view_index(0));
            def.nodes.add_leaf(root_node, cc::move(leaf));

            auto root = sv::view_data{};
            root.id = sv::view_id::from_string("root");
            root.layers.push_back(
                {.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
            def.root_view = sv::view_index(def.views.size());
            def.views.push_back(cc::move(root));
        }

        auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                              .width = output_size[0],
                                                              .height = output_size[1],
                                                              .usage = sg::texture_usage::render_target});
        auto store = sv::view_store{};

        // Frames until the mean holds two, which a view restarting on every frame never reaches.
        auto index = u64(0);
        auto const accumulated = sv_test::frames_until_executed(
            ctx,
            [&](sg::command_list& cmd)
            {
                resources.advance_to(ctx.current_epoch());
                store.begin_frame(u64(ctx.current_epoch()));
                auto const plan = sv::build_render_plan(def, output_size, index++, {});
                (void)sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                                   output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                return store.accumulated_frames(traced_id) >= 2 ? sg::routine_outcome::executed
                                                                : sg::routine_outcome::declined;
            });
        CHECK(accumulated)
            .context(cc::format("{}: {} frames accumulated",
                                mode == sv::render_mode::wavefront ? "wavefront" : "megakernel",
                                store.accumulated_frames(traced_id)));
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// A camera cut, down the plan path, on a nested view whose refresh is throttled.
//
// The request is sticky: a frame that does not trace the view leaves it standing, and the next frame that does trace
// consumes it.
// What it drops is observable on the temporal member's own history, and only on a frame that member does not run —
// a run would consume the reset in the same frame that made it.
ASYNC_INVOCABLE_TEST("sv - a camera cut waits for a traced frame and drops the temporal history",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cloud", cloud.positions, cloud.materials));
    resources.wait_for_pending_uploads();

    auto const output_size = tg::vec2i(64, 64);
    auto const cut_id = sv::view_id::from_string("cut");

    // The temporal member owns the first two accumulated frames and hands over in one step after them, so a third
    // frame is one it does not run.
    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = cut_id;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        v.refresh = {.rate = 0.0f}; // only when the history says there is nothing to re-present
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.settings.reconstruct.denoiser = sr::denoise_method::automatic;
        scene.settings.temporal_denoise_frames = 2;
        scene.settings.temporal_denoise_fade_frames = 0;
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

    // An empty history forces the nested view to trace; one saying its image exists and is current throttles it.
    auto const traced = sv::view_history{};
    auto throttled = sv::view_history{};

    auto const execute = [&](sg::command_list& cmd, u64 index, sv::view_history const& history)
    {
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index, history);
        auto const outcome = sv::viewer_renderer::execute(
            cmd, def, plan, resources, store, output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        return cc::pair(plan, outcome);
    };

    // A coroutine lambda, awaited on the spot, so what it captures by reference outlives every suspend.
    auto const frame = [&](u64 index, sv::view_history const& history) -> cc::shared_async<sv::render_plan>
    {
        auto cmd = ctx.create_command_list();
        auto [plan, outcome] = execute(*cmd, index, history);
        CHECK(outcome == sg::routine_outcome::executed);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        co_return plan;
    };

    // Both denoisers and the tracer warmed first, so every counted frame below is one that dispatched.
    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float); // the format a denoised slot declares
    (void)co_await ctx.routines.idle_completion();
    REQUIRE(sv_test::frames_until_executed(ctx, [&](sg::command_list& cmd) { return execute(cmd, 0, traced).second; }));
    // A trace executes while a permutation still compiles behind a stand-in, and every such frame restarts the mean.
    // So the warm-up lasts until two frames accumulate, which a slow compile beside a busy suite otherwise outlasts.
    REQUIRE(sv_test::frames_until_executed(ctx,
                                           [&](sg::command_list& cmd)
                                           {
                                               auto const outcome = execute(cmd, 0, traced).second;
                                               return store.accumulated_frames(cut_id) >= 2
                                                        ? outcome
                                                        : sg::routine_outcome::declined;
                                           }));
    store = sv::view_store{}; // the warm-up's own counts are not what this test is about

    auto const frame_history = [&]() -> sr::reconstruct_history const&
    {
        auto const* const rec = store.peek_ptr(cut_id);
        CC_ASSERT(rec != nullptr, "the view traced, so it has a record");
        auto const* const slot = rec->temporal.get_ptr(sv::temporal_id::frame_samples(0));
        CC_ASSERT(slot != nullptr, "a layer that may denoise temporally declares its frame samples");
        return slot->denoise;
    };

    // Two temporal frames, then one past the hand-off.
    auto const first = co_await frame(1, traced);
    REQUIRE(first.traces.size() == 1);
    (void)co_await frame(2, traced);
    (void)co_await frame(3, traced);
    REQUIRE(store.accumulated_frames(cut_id) == 3);
    CHECK(!frame_history().is_reset_pending()); // the temporal runs consumed the reset a new scene asks for

    throttled.entries[cut_id] = {.exists = true, .resolution = first.traces[0].resolution, .last_refresh_frame = 4};
    store.get_or_create(cut_id).camera_cut_pending = true;

    // A frame that does not trace the view leaves the request standing.
    auto const skipped = co_await frame(4, throttled);
    REQUIRE(skipped.traces.size() == 1);
    CHECK(!skipped.traces[0].refresh);
    CHECK(store.peek_ptr(cut_id)->camera_cut_pending);
    CHECK(store.accumulated_frames(cut_id) == 3);

    // The next traced frame consumes it and drops the temporal history, and restarts no accumulation of its own.
    (void)co_await frame(5, traced);
    CHECK(!store.peek_ptr(cut_id)->camera_cut_pending);
    CHECK(frame_history().is_reset_pending());
    CHECK(store.accumulated_frames(cut_id) == 4);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A layer upscaling below native, down the plan path: a frame whose image changed is traced at the upscaler's input
// extent and upscaled into the denoised slot, and is no sample of the mean.
// The first frame the camera holds still restarts the mean at full size, and the next one accumulates onto it.
ASYNC_INVOCABLE_TEST("sv - a moving camera traces at the upscaler's extent and a still one at full size",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");
    if (!sr::query_reconstruct_support(ctx).fsr)
        SKIP("no upscaler on this device");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cloud", cloud.positions, cloud.materials));
    resources.wait_for_pending_uploads();

    auto const output_size = tg::vec2i(64, 64);
    auto const moving_id = sv::view_id::from_string("moving");

    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = moving_id;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.settings.reconstruct.denoiser = sr::denoise_method::automatic;
        scene.settings.reconstruct.scale = sr::render_scale_preset::performance;
        // A move ends on the first still frame, so what each frame traces does not depend on the clock; the hold is tested below.
        scene.settings.motion_hold_ms = 0.0f;
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
    auto const history = sv::view_history{};
    auto step = 0.0;

    // Moves the camera first when asked, so the frame sees a different image than the last one did.
    auto const execute = [&](sg::command_list& cmd, u64 index, bool move)
    {
        if (move)
        {
            step += 0.05;
            def.views[0].camera.position = tg::pos3d(2.4 + step, 1.8, -3.2);
        }
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index, history);
        return sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                            output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    };

    auto const frame = [&](u64 index, bool move) -> cc::shared_async<cc::unit>
    {
        auto cmd = ctx.create_command_list();
        CHECK(execute(*cmd, index, move) == sg::routine_outcome::executed);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    };

    // Warmed moving and still, so both the upscaler and the full-size members have compiled before anything is counted.
    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float); // the format a denoised slot declares
    (void)co_await ctx.routines.idle_completion();
    REQUIRE(sv_test::frames_until_executed(ctx, [&](sg::command_list& cmd) { return execute(cmd, 0, true); }));

    auto const accumulation = [&]() -> sv::impl::temporal_slot const&
    {
        auto const* const rec = store.peek_ptr(moving_id);
        CC_ASSERT(rec != nullptr, "the view traced, so it has a record");
        return *rec->temporal.get_ptr(sv::temporal_id::accumulation(0));
    };

    // A frame shaded by a stand-in counts as a changed image, so until every permutation has compiled a still camera
    // traces scaled too; held still until the mean climbs, which is when none is left.
    auto warm_frames = 0;
    while (accumulation().accum_frame < 2)
    {
        (void)co_await frame(0, false);
        ++warm_frames;
        REQUIRE(warm_frames < 500);
    }
    auto const frame_samples = [&]() -> sv::impl::temporal_slot const&
    { return *store.peek_ptr(moving_id)->temporal.get_ptr(sv::temporal_id::frame_samples(0)); };

    // The move's first frame ends a still run that drew samples, so it moves the seed on once.
    // The scaled frames after it draw nothing through the mean's sequence, so they leave the count as it is.
    auto const restarts_before = accumulation().restarts;
    for (auto i = u64(1); i <= 3; ++i)
    {
        (void)co_await frame(i, true);
        CHECK(accumulation().traced_scaled);
        CHECK(accumulation().accum_frame == 0);
    }
    CHECK(accumulation().restarts == restarts_before + 1);
    auto const input = frame_samples().scaled_denoise.extent();
    CHECK(input[0] < output_size[0]);
    CHECK(input[1] < output_size[1]);
    CHECK(accumulation().scaled_frame >= 3);

    // The upscaler's own extents are its history's business; what the view presents is the denoised slot, which a
    // landed upscale marks current.
    CHECK(store.peek_ptr(moving_id)->temporal.get_ptr(sv::temporal_id::denoised(0))->accum_frame == 1);

    (void)co_await frame(4, false);
    CHECK(!accumulation().traced_scaled);
    CHECK(accumulation().accum_frame == 1);
    CHECK(accumulation().restarts == restarts_before + 1); // a sequence apart from the still run before the move
    CHECK(frame_samples().denoise.extent() == output_size);

    (void)co_await frame(5, false);
    CHECK(accumulation().accum_frame == 2);

    // Within a move's hold a frame that holds still stays in the moving stream, so a pause mid-move keeps its history.
    sv::ensure_scene_3d(def.views[0]).settings.motion_hold_ms = 1e9f;
    (void)co_await frame(6, true);
    CHECK(accumulation().traced_scaled);
    auto const scaled_before = accumulation().scaled_frame;
    (void)co_await frame(7, false);
    CHECK(accumulation().traced_scaled);
    CHECK(accumulation().scaled_frame == scaled_before + 1);
    CHECK(accumulation().accum_frame == 0);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A layer at its sample target traces nothing, down the plan path, so what it would otherwise write beside the mean is never written.
//
// Guides first asked for there — a denoiser turned on — would stay blank, and the denoiser would run once on them and keep that.
// So the mean starts over, and the frame that restarts it writes the guides.
// And a change of denoise settings there denoises once more, where a frame at the target otherwise keeps what was last denoised.
ASYNC_INVOCABLE_TEST("sv - a layer at its target restarts for new guides and denoises again on new settings",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cloud", cloud.positions, cloud.materials));
    resources.wait_for_pending_uploads();

    auto const output_size = tg::vec2i(64, 64);
    auto const view_id = sv::view_id::from_string("at target");

    // The megakernel at one sample a frame, so the target is two traced frames.
    auto def = sv::viewer_definition{};
    def.sample_target = 2;
    {
        auto v = sv::view_data{};
        v.id = view_id;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.settings.mode = sv::render_mode::megakernel;
        scene.settings.samples_per_pixel = 1;
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
    auto& settings = sv::ensure_scene_3d(def.views[0]).settings;

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});

    auto store = sv::view_store{};
    auto const history = sv::view_history{};

    // A declined frame is fine here: a hit group or a denoiser still compiling declines, and the loops below wait on state rather than on outcomes.
    auto const frame = [&](u64 index) -> cc::shared_async<cc::unit>
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index, history);
        (void)sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                           output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    };

    auto const slot_of = [&](u64 temporal) -> sv::impl::temporal_slot*
    {
        auto* const rec = store.get_ptr(view_id);
        return rec == nullptr ? nullptr : rec->temporal.get_ptr(temporal);
    };
    auto const accumulation = [&]() -> sv::impl::temporal_slot const&
    {
        auto const* const slot = slot_of(sv::temporal_id::accumulation(0));
        CC_ASSERT(slot != nullptr, "the view traced, so it has an accumulator");
        return *slot;
    };

    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float); // the format a denoised slot declares
    (void)co_await ctx.routines.idle_completion();

    // Until the mean holds the target, which a stand-in still compiling restarts on the way.
    // A cold shader compile runs as background work the loop has to drive, so the deadline is in seconds rather than frames.
    auto index = u64(1);
    auto const start = cc::current_time_steady_secs();
    while (store.min_accumulated_samples(view_id) < 2)
    {
        (void)co_await frame(index++);
        REQUIRE(cc::current_time_steady_secs() - start < 240.0);
        sv_test::drive_ambient_work();
    }

    // At the target nothing traces, so nothing counts.
    auto const frames_at_target = accumulation().accum_frame;
    (void)co_await frame(index++);
    (void)co_await frame(index++);
    CHECK(accumulation().accum_frame == frames_at_target);

    // A denoiser turned on there asks for guides no frame would write, so the mean starts over and the restarting frame writes them.
    settings.reconstruct.denoiser = sr::denoise_method::atrous;
    (void)co_await frame(index++);
    auto const guide_frames = [&]
    {
        auto const* const slot = slot_of(sv::temporal_id::normal_guide(0));
        return slot == nullptr ? u32(0) : slot->accum_frame;
    };
    CHECK(accumulation().accum_frame == 1);
    CHECK(guide_frames() == 1);

    // Back to the target, denoised.
    auto const denoised = [&]() -> sv::impl::temporal_slot&
    {
        auto* const slot = slot_of(sv::temporal_id::denoised(0));
        CC_ASSERT(slot != nullptr, "a denoising layer declares its denoised image");
        return *slot;
    };
    // At the target a frame traces nothing and costs next to nothing, so a frame count would run out while the denoiser still compiles.
    while (store.min_accumulated_samples(view_id) < 2 || denoised().accum_frame == 0)
    {
        (void)co_await frame(index++);
        REQUIRE(cc::current_time_steady_secs() - start < 240.0);
        sv_test::drive_ambient_work();
    }
    CHECK(guide_frames() == accumulation().accum_frame); // the guides hold every frame of this mean

    // A reset only a denoise consumes, which says whether one ran.
    // At the target with nothing changed, none does.
    denoised().denoise.reset();
    (void)co_await frame(index++);
    CHECK(denoised().denoise.is_reset_pending());

    // A changed denoise setting denoises once more, and restarts nothing of the mean.
    auto const frames_before = accumulation().accum_frame;
    settings.reconstruct.sharpness = 0.8f;
    (void)co_await frame(index++);
    CHECK(!denoised().denoise.is_reset_pending());
    CHECK(denoised().accum_frame == 1);
    CHECK(accumulation().accum_frame == frames_before);

    co_await cc::async_settled(sv::background_work(ctx));
}

// The wavefront's paths live on the layer's accumulation slot and carry across still frames, all under one generation.
// A moved camera restarts them under the next, a frame budget paces a still frame by its own timestamps, and leaving the
// wavefront releases them.
ASYNC_INVOCABLE_TEST("sv - a wavefront layer carries its paths across still frames down the plan path",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");
    if (!ctx.supports(sg::feature::ray_query))
        SKIP("the wavefront tracer needs inline ray queries");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("carried cloud", cloud.positions, cloud.materials));
    resources.wait_for_pending_uploads();

    auto const output_size = tg::vec2i(64, 64);
    auto const traced_id = sv::view_id::from_string("carried");
    auto const definition = [&](tg::pos3d eye, sv::render_mode mode)
    {
        auto def = sv::viewer_definition{};
        auto v = sv::view_data{};
        v.id = traced_id;
        v.camera = sv::camera{.position = eye};
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.settings.mode = mode;
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

        // What a viewer sets from its config, so a still frame is paced by the GPU's own timing.
        def.wavefront_budget_ms = 16.0f;
        return def;
    };

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    auto store = sv::view_store{};
    auto index = u64(0);
    auto const until = [&](sv::viewer_definition const& def, auto&& done)
    {
        return sv_test::frames_until_executed(
            ctx,
            [&](sg::command_list& cmd)
            {
                resources.advance_to(ctx.current_epoch());
                store.begin_frame(u64(ctx.current_epoch()));
                auto const plan = sv::build_render_plan(def, output_size, index++, {});
                (void)sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                                   output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                return done() ? sg::routine_outcome::executed : sg::routine_outcome::declined;
            });
    };
    auto const slot = [&]() -> sv::impl::temporal_slot const&
    {
        auto const* const rec = store.peek_ptr(traced_id);
        CC_ASSERT(rec != nullptr, "the view traced, so it has a record");
        auto const* const s = rec->temporal.get_ptr(sv::temporal_id::accumulation(0));
        CC_ASSERT(s != nullptr, "a traced layer declares its accumulator");
        return *s;
    };

    // Until the cloud's own shade kernel has landed, since a stand-in still compiling restarts the mean every frame.
    auto const still = definition(tg::pos3d(2.4, 1.8, -3.2), sv::render_mode::wavefront);
    REQUIRE(until(still, [&] { return store.accumulated_frames(traced_id) >= 2; }));
    REQUIRE(slot().paths.has_value());
    auto const generation = slot().paths.value().generation;
    CHECK(generation != 0u);

    // Still frames carry: the mean counts each one, and no path restarts under a new generation.
    for (auto i = 0; i < 3; ++i)
    {
        auto const before = store.accumulated_frames(traced_id);
        REQUIRE(until(still, [&] { return store.accumulated_frames(traced_id) == before + 1; }));
        CHECK(slot().paths.value().generation == generation);
    }

    // The budget paces a still frame once its timestamps land, which only a device that times its queue has.
    auto probe = ctx.create_command_list();
    auto const timed = probe->query.is_supported();
    ctx.drop_command_list(cc::move(probe));
    if (timed)
        CHECK(until(still, [&] { return slot().pacing.seconds_per_full_round > 0.0; }));

    // A budget no round fits runs one round, starting samples in only a share of the free slots, which each measured frame lowers.
    // wavefront-trace-test.cc pins that a partial fill keeps the image's mean; this pins that the viewer lowers it at all.
    if (timed)
    {
        auto starved = definition(tg::pos3d(2.4, 1.8, -3.2), sv::render_mode::wavefront);
        starved.wavefront_budget_ms = 0.001f;
        CHECK(until(starved, [&] { return slot().pacing.fill < 0.5f; }));
    }

    // A moved camera restarts the mean, and its paths under the next generation.
    auto const moved = definition(tg::pos3d(2.6, 1.8, -3.2), sv::render_mode::wavefront);
    REQUIRE(until(moved, [&] { return store.accumulated_frames(traced_id) == 1; }));
    CHECK(slot().paths.value().generation > generation);

    // The megakernel keeps no paths, so a layer switched to it releases them.
    auto const megakernel = definition(tg::pos3d(2.6, 1.8, -3.2), sv::render_mode::megakernel);
    REQUIRE(until(megakernel, [&] { return store.accumulated_frames(traced_id) >= 1 && !slot().paths.has_value(); }));

    co_await cc::async_settled(sv::background_work(ctx));
}
