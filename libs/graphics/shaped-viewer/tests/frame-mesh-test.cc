#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
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

    // its record names its own vertices, not the placeholder's
    auto const& mine = resources.acquire_frame_mesh(*cmd, a.geometry, true);
    auto const record = resources.describe_instance(*cmd, mine, item.instance);
    CHECK(record.vertices == u32(resources.acquire_buffer(mine.vertices.raw()->as_raw_readonly())));
    CHECK(record.vertices
          != u32(resources.acquire_buffer(resources.meshes.placeholder_vertices().raw()->as_raw_readonly())));
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
