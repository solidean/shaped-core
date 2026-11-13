#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Meshes placed with mesh_lifetime::frame: uploaded inline for one epoch, never streamed, never a placeholder.

namespace
{
/// A cloud of triangles moved by `dx`, with no attributes, so nothing about it streams.
sv::mesh moved_cloud(float dx)
{
    auto positions = sv_test::make_triangle_cloud(32).positions;
    for (auto& p : positions)
        p[0] += dx;
    return sv::mesh{.name = "cloud", .geometry = sv::triangle_geometry::create_from_positions(positions)};
}

[[nodiscard]] bool supports_tracing(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported();
    ctx.drop_command_list(cc::move(probe));
    return supported;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a frame mesh uploads once per epoch, names its own buffers, and is dropped with the epoch",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!supports_tracing(ctx))
        SKIP("device reports no ray tracing support");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const a = moved_cloud(0);
    auto const b = moved_cloud(1);
    auto const item = resources.acquire_frame_scene_item(a);
    CHECK(item.lifetime == sv::mesh_lifetime::frame);
    CHECK(item.mesh == sv::mesh_id::invalid);
    CHECK(resources.settling_count() == 0); // nothing was handed to the stream

    resources.advance_to(ctx.current_epoch());
    auto cmd = ctx.create_command_list();
    auto const& first = resources.acquire_frame_mesh(*cmd, a.geometry, true);
    auto const first_vertices = first.vertices.raw();
    auto const first_blas = first.blas;
    CHECK(first_blas != nullptr);
    CHECK(first.triangle_count == 32);

    // the same content again in this epoch is the same upload and the same BLAS; other content is its own
    auto const& again = resources.acquire_frame_mesh(*cmd, a.geometry, true);
    CHECK(again.vertices.raw() == first_vertices);
    CHECK(again.blas == first_blas);
    auto const& other = resources.acquire_frame_mesh(*cmd, b.geometry, true);
    CHECK(other.vertices.raw() != first_vertices);

    // its record names its own buffers from their start, not the central pool every resident mesh and the placeholder share
    auto const& mine = resources.acquire_frame_mesh(*cmd, a.geometry, true);
    auto const record = resources.describe_instance(*cmd, mine, item.instance);
    CHECK(record.vertices == u32(resources.acquire_buffer(mine.vertices.raw()->as_raw_readonly())));
    CHECK(record.indices == u32(resources.acquire_buffer(mine.indices.raw()->as_raw_readonly())));
    CHECK(record.base_vertex == 0u);
    CHECK(record.first_index == 0u);
    CHECK(record.vertices != u32(resources.acquire_buffer(resources.meshes.vertex_pool().raw()->as_raw_readonly())));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    (void)co_await ctx.idle_completion();

    // a new epoch uploads anew
    resources.advance_to(ctx.current_epoch());
    auto next = ctx.create_command_list();
    auto const& later = resources.acquire_frame_mesh(*next, a.geometry, false);
    CHECK(later.blas == nullptr); // not asked for, so not built
    ctx.submit_command_list(cc::move(next));
    ctx.advance_epoch();
    (void)co_await ctx.idle_completion();
    CHECK(resources.settling_count() == 0);
}

ASYNC_INVOCABLE_TEST("sv - a frame mesh's new attributes are resident in the frame it is placed, never streamed",
                     (sg::context_handle const& ctx_h))
{
    // Each frame places a mesh whose per-triangle color is new content, as an edited mesh's would be.
    // Streamed, such an attribute stayed pending for a frame or more, and the mesh was shaded by the fallback meanwhile.
    auto& ctx = *ctx_h;
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const colored = [](f32 shade)
    {
        auto m = moved_cloud(0);
        auto colors = cc::vector<tg::vec3f>();
        for (auto t = 0; t < 32; ++t)
            colors.push_back(tg::vec3f(shade, 0.5f, f32(t) / 32));
        m.attributes.push_back(
            sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colors)));
        return m;
    };
    auto fallback_frames = 0;
    for (auto frame = 0; frame < 8; ++frame)
    {
        resources.advance_to(ctx.current_epoch());
        auto const item = resources.acquire_frame_scene_item(colored(0.1f * f32(frame)));
        CHECK(resources.settling_count() == 0); // nothing was handed to the stream
        auto cmd = ctx.create_command_list();
        resources.attributes.record_inline_uploads(*cmd);
        fallback_frames += resources.attributes_resident(item.instance) ? 0 : 1;
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
    }
    CHECK(fallback_frames == 0);
    CHECK(resources.attributes.inline_count() == 0);
    (void)co_await ctx.idle_completion();
}

ASYNC_INVOCABLE_TEST("sv - a frame mesh traces in the frame it is placed, and keeps the image only while its content "
                     "holds",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!supports_tracing(ctx))
        SKIP("device reports no ray tracing support");
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto store = sv::view_store{};
    auto const view_with = [&](sv::mesh const& m)
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("frame-mesh");
        v.resolution = tg::vec2i(64, 64);
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        sv::ensure_scene_3d(v).items.push_back(resources.acquire_frame_scene_item(m));
        return v;
    };
    auto const id = sv::view_id::from_string("frame-mesh");
    auto const accumulated = [&] { return store.accumulated_frames(id); };
    // one traced frame: driven until the accumulator changes, so a frame that declined is not counted (as view-accumulation-test does)
    auto const trace = [&](sv::view_data const& v)
    {
        auto const before = accumulated();
        return sv_test::frames_until_executed(ctx,
                                              [&](sg::command_list& cmd)
                                              {
                                                  resources.advance_to(ctx.current_epoch());
                                                  store.begin_frame(u64(ctx.current_epoch()));
                                                  (void)sv::view_renderer::execute(cmd, v, resources, store);
                                                  return accumulated() != before ? sg::routine_outcome::executed
                                                                                 : sg::routine_outcome::declined;
                                              });
    };

    // A frame shaded through a stand-in for a permutation still compiling restarts the mean, so the shaders are built first:
    // traced until two frames accumulate.
    REQUIRE(sv_test::frames_until_executed(
        ctx,
        [&](sg::command_list& cmd)
        {
            resources.advance_to(ctx.current_epoch());
            store.begin_frame(u64(ctx.current_epoch()));
            (void)sv::view_renderer::execute(cmd, view_with(moved_cloud(0)), resources, store);
            return accumulated() >= 2 ? sg::routine_outcome::executed : sg::routine_outcome::declined;
        }));

    // the same content placed anew every frame, from a new mesh each time, accumulates: the image holds
    auto const settled = accumulated();
    for (auto i = 0; i < 3; ++i)
        REQUIRE(trace(view_with(moved_cloud(0))));
    CHECK(accumulated() == settled + 3);
    CHECK(resources.settling_count() == 0);

    // new content restarts it, and still never streams; traced twice each, since a restart from 1 to 1 is no change to wait for
    for (auto i = 1; i <= 3; ++i)
    {
        REQUIRE(trace(view_with(moved_cloud(0.1f * f32(i)))));
        CHECK(accumulated() == 1);
        REQUIRE(trace(view_with(moved_cloud(0.1f * f32(i)))));
        CHECK(accumulated() == 2);
    }
    CHECK(resources.settling_count() == 0);

    // New buffers every frame for many frames: each epoch's bindless indices are acquired, not minted, so the descriptor heap stays flat.
    // A per-frame transient buffer minted persistently exhausted it within a second once (gpu_resource_manager.hh, describe_instance).
    for (auto i = 0; i < 300; ++i)
        REQUIRE(sv_test::frames_until_executed(ctx,
                                               [&](sg::command_list& cmd)
                                               {
                                                   resources.advance_to(ctx.current_epoch());
                                                   store.begin_frame(u64(ctx.current_epoch()));
                                                   (void)sv::view_renderer::execute(
                                                       cmd, view_with(moved_cloud(0.001f * f32(i))), resources, store);
                                                   return sg::routine_outcome::executed;
                                               }));

    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a changing scene restarts the mean, and only a cut restarts the temporal history",
                     (sg::context_handle const& ctx_h))
{
    // A simulation draws its body anew every frame, and the temporal denoiser is what keeps that image stable.
    // Restarting its history on every frame threw away the whole view's, the static ground with it, and the image flickered.
    auto& ctx = *ctx_h;
    if (!supports_tracing(ctx))
        SKIP("device reports no ray tracing support");
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the shaders");

    // recolored per triangle, as a simulation's drawn attributes are, and moved by `dx`
    auto const body = [](f32 dx, f32 shade)
    {
        auto m = moved_cloud(dx);
        auto colors = cc::vector<tg::vec3f>();
        for (auto t = 0; t < 32; ++t)
            colors.push_back(tg::vec3f(shade, 0.5f, f32(t) / 32));
        m.attributes.push_back(
            sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colors)));
        return m;
    };

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto store = sv::view_store{};
    auto const id = sv::view_id::from_string("deforming");
    auto const output_size = tg::vec2i(64, 64);
    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    // down the plan path, which declares the temporal slots a layer's denoiser needs
    auto const definition_with = [&](cc::span<sv::mesh const> meshes, tg::vec3f offset, f32 lamp_nits = 0.0f)
    {
        auto def = sv::viewer_definition{};
        auto v = sv::view_data{};
        v.id = id;
        v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)};
        auto& scene = sv::ensure_scene_3d(v);
        scene.settings.mode = sv::render_mode::hybrid; // what the viewer resolves `automatic` to on a device that traces
        scene.settings.reconstruct.denoiser = sr::denoise_method::automatic;
        for (auto const& m : meshes)
        {
            auto item = resources.acquire_frame_scene_item(m);
            item.transform = tg::affine_transform3f::make_trs(offset, tg::quat_f::identity, tg::vec3f(1, 1, 1));
            scene.items.push_back(item);
        }
        if (lamp_nits > 0.0f)
            scene.lights.push_back(
                {.id = sv::light_id::from_string("lamp"),
                 .light
                 = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(0.75f, 0, 0), tg::vec3f(0, 0, 0.75f)).nits(lamp_nits)});
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
        return def;
    };
    auto index = u64(0);
    auto const execute = [&](sg::command_list& cmd, sv::viewer_definition const& def)
    {
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index++, sv::view_history{});
        (void)sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                           output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    };
    auto const slot = [&](u64 t) -> sv::impl::temporal_slot const*
    {
        auto const* const rec = store.peek_ptr(id);
        return rec == nullptr ? nullptr : rec->temporal.get_ptr(t);
    };
    // where the mean is: (the image it averages, frames), which every traced frame moves, a restart included
    auto const progress = [&]
    {
        auto const* const s = slot(sv::temporal_id::accumulation(0));
        return s == nullptr ? cc::pair<u64, u32>{0, 0} : cc::pair<u64, u32>{s->reset_hash, s->accum_frame};
    };
    auto const trace = [&](sv::viewer_definition const& def)
    {
        auto const before = progress();
        return sv_test::frames_until_executed(ctx,
                                              [&](sg::command_list& cmd)
                                              {
                                                  execute(cmd, def);
                                                  return progress() != before ? sg::routine_outcome::executed
                                                                              : sg::routine_outcome::declined;
                                              });
    };

    // the denoisers and the shaders built first, so no frame below shades through a stand-in
    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float); // the format a denoised slot declares
    (void)co_await ctx.routines.idle_completion();
    auto const still = definition_with(cc::vector<sv::mesh>{body(0, 0)}, tg::vec3f(0, 0, 0));
    REQUIRE(sv_test::frames_until_executed(ctx,
                                           [&](sg::command_list& cmd)
                                           {
                                               execute(cmd, still);
                                               return progress().second >= 2 ? sg::routine_outcome::executed
                                                                             : sg::routine_outcome::declined;
                                           }));
    REQUIRE(slot(sv::temporal_id::frame_samples(0))
            != nullptr); // a layer that may denoise temporally declares its frame samples
    auto const restarts = [&] { return slot(sv::temporal_id::frame_samples(0))->denoise.restarts(); };
    auto const built = restarts();
    REQUIRE(built >= 1); // the temporal member ran, so the checks below watch a history that exists

    // deformed, recolored and moved every frame: the mean restarts each time, the history never does
    for (auto i = 1; i <= 8; ++i)
    {
        auto const image = progress().first;
        auto const deformed = cc::vector<sv::mesh>{body(0.05f * f32(i), i <= 4 ? 0.0f : 0.1f * f32(i))};
        REQUIRE(trace(definition_with(deformed, tg::vec3f(0.02f * f32(i), 0, 0))));
        CHECK(progress().first != image);
        CHECK(progress().second == 1);
        CHECK(restarts() == built).context(cc::format("frame {}", i));
    }

    // an object or a light coming, and a light changing, are content too: a video's history must survive them
    auto const two = cc::vector<sv::mesh>{body(0, 0), body(1, 0)};
    REQUIRE(trace(definition_with(two, tg::vec3f(0, 0, 0))));
    CHECK(restarts() == built);
    REQUIRE(trace(definition_with(two, tg::vec3f(0, 0, 0), 12.0f)));
    CHECK(restarts() == built);
    REQUIRE(trace(definition_with(two, tg::vec3f(0, 0, 0), 30.0f)));
    CHECK(restarts() == built);

    // only a cut restarts it, which is how a caller replacing the scene outright says so
    store.get_or_create(id).camera_cut_pending = true;
    REQUIRE(trace(definition_with(two, tg::vec3f(0.1f, 0, 0), 30.0f)));
    CHECK(restarts() == built + 1);

    co_await cc::async_settled(sv::background_work(ctx));
}
