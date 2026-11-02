#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// sv::mesh_manager's central pools: every mesh is a range of one vertex buffer and one index buffer.
// What is held here is what the pools add over per-mesh buffers: growth keeps every mesh's bytes, and an evicted range is reused only once its frames retire.

namespace
{
/// A raw triangle list of `vertex_count` positions, each one telling which mesh and which vertex it is.
[[nodiscard]] cc::vector<tg::pos3f> numbered_positions(int mesh, isize vertex_count)
{
    auto positions = cc::vector<tg::pos3f>();
    positions.reserve(vertex_count);
    for (auto i = isize(0); i < vertex_count; ++i)
        positions.push_back(tg::pos3f(float(mesh), float(i), 0.5f * float(i)));
    return positions;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a geometry pool that grows keeps every mesh's vertices and indices",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    auto meshes = sv::mesh_manager::create(ctx);

    // Acquired back to back, so later meshes grow the pools while earlier ones may still be streaming.
    // Five raw lists of 20,001 vertices outgrow the initial pool, and one indexed mesh goes in between them.
    constexpr isize vertex_count = 20'001;
    auto const initial_capacity = meshes.vertex_pool_capacity();
    auto ids = cc::vector<sv::mesh_id>();
    for (auto m = 0; m < 5; ++m)
        ids.push_back(meshes.acquire(sv::triangle_data::create(numbered_positions(m, vertex_count))));

    auto const reversed = cc::vector<u32>{2, 1, 0, 3, 5, 4};
    auto const indexed = meshes.acquire(sv::indexed_triangle_data::create(numbered_positions(9, 6), reversed));

    CHECK(meshes.vertex_pool_capacity() > initial_capacity);
    CHECK(meshes.vertex_pool_capacity() >= 5 * vertex_count + 6);

    meshes.wait_for_settled();
    for (auto const id : ids)
        REQUIRE(meshes.get(id).state == sv::residency::complete);
    REQUIRE(meshes.get(indexed).state == sv::residency::complete);

    auto cmd = ctx.create_command_list();
    auto vertex_readback = cmd->download.data_from_buffer(meshes.vertex_pool());
    auto index_readback = cmd->download.data_from_buffer(meshes.index_pool());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const vertices = co_await vertex_readback.data();
    auto const indices = co_await index_readback.data();

    for (auto m = 0; m < 5; ++m)
    {
        auto const& g = meshes.get(ids[m]).geometry;
        REQUIRE(g.vertex_count == vertex_count);
        REQUIRE(g.index_count() == vertex_count);
        auto const expected = numbered_positions(m, vertex_count);
        auto mismatched = isize(0);
        for (auto i = isize(0); i < vertex_count; ++i)
        {
            if (vertices[g.base_vertex + i] != expected[i])
                ++mismatched;
            // A raw list is uploaded with the indices 0, 1, 2, ..., relative to its own base vertex.
            if (indices[g.first_index + i] != u32(i))
                ++mismatched;
        }
        CHECK(mismatched == 0).context(cc::format("mesh {}: {} vertices or indices differ", m, mismatched));
    }

    auto const& g = meshes.get(indexed).geometry;
    auto const expected = numbered_positions(9, 6);
    for (auto i = isize(0); i < 6; ++i)
    {
        CHECK(vertices[g.base_vertex + i] == expected[i]);
        CHECK(indices[g.first_index + i] == reversed[i]);
    }

    co_await ctx.idle_completion();
}

ASYNC_INVOCABLE_TEST("sv - an evicted mesh's range is reused only once its frames have retired",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    auto meshes = sv::mesh_manager::create(ctx);

    auto const first = meshes.acquire(sv::triangle_data::create(numbered_positions(0, 30)));
    meshes.wait_for_settled();
    auto const evicted = meshes.get(first).geometry;
    CHECK(meshes.evict(first));

    // The epoch it was evicted in has not even closed, so a frame may still read its range.
    auto const second = meshes.acquire(sv::triangle_data::create(numbered_positions(1, 30)));
    CHECK(meshes.get(second).geometry.base_vertex != evicted.base_vertex);
    CHECK(meshes.get(second).geometry.first_index != evicted.first_index);

    // Once the GPU has finished that epoch, the range is free again, and first fit finds it.
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    auto const third = meshes.acquire(sv::triangle_data::create(numbered_positions(2, 30)));
    CHECK(meshes.get(third).geometry.base_vertex == evicted.base_vertex);
    CHECK(meshes.get(third).geometry.first_index == evicted.first_index);

    meshes.wait_for_settled();
    ctx.advance_epoch();
    co_await ctx.idle_completion();
}
