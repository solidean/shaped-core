#include "viewer_test_env.hh"

#include <clean-core/container/vector.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/resources/quadric_data.hh>
#include <shaped-viewer/resources/resource_managers.hh>

using namespace cc::primitive_defines;

// sv::quadric_manager: the streamed upload of a quadric batch and the procedural BLAS built behind it.
//
// This needs a ray-tracing device, because the thing being tested IS the acceleration structure build — a procedural BLAS over
// AABBs rather than triangles, which is the first use of that path anywhere in sv.
// What the batch is made of, and what its content hash promises, is covered without a device in quadric-set-test.cc.

namespace
{
/// A small batch of both primitive kinds, built the way a caller would.
sv::quadric_set structure_of(float radius)
{
    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), radius));
    set.add_sphere(tg::sphere3f(tg::pos3f(1, 0, 0), radius));
    set.add_line(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), radius * 0.5f);
    return set;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a quadric batch streams in and gets a procedural BLAS", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    auto manager = sv::quadric_manager::create(ctx);

    auto const set = structure_of(0.25f);
    auto const id = manager.acquire(sv::quadric_data::of(set));

    // An acquire hands both payloads to the streaming actor and mints an id; nothing is resident yet.
    //
    // Deterministic despite the transfers running on another thread: a record's state advances only where the settle pass runs,
    // so what the copy queue has managed by now cannot change what this observes.
    CHECK(manager.get(id).state == sv::residency::pending);
    CHECK(manager.settling_count() == 1);

    // A pending batch has no acceleration structure of its own, which is why a placeholder has to stand in for it.
    CHECK(manager.get(id).blas == nullptr);

    // The summary crosses at acquire rather than on arrival, which is what lets that placeholder be SIZED before the
    // primitives it stands in for exist on the GPU at all.
    REQUIRE(manager.get(id).bounds.has_value());
    CHECK(manager.get(id).bounds.value().max[0] > 1.0f);
    CHECK(manager.get(id).primitive_count == 3);

    // Waiting is what a caller with no frame loop does; a viewer instead drains what has landed, once per frame.
    manager.wait_for_settled();

    CHECK(manager.get(id).state == sv::residency::complete);
    CHECK(manager.settling_count() == 0);
    CHECK(manager.get(id).blas != nullptr);

    // Both buffers are real, and they are separate: the boxes are the build input and the primitives are the
    // intersection shader's, which is why a batch is two buffers rather than one.
    CHECK(manager.get(id).aabbs.element_count() == 3);
    CHECK(manager.get(id).primitives.element_count() == 3);

    co_return;
}

ASYNC_INVOCABLE_TEST("sv - an unchanged quadric batch is acquired without uploading", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    auto manager = sv::quadric_manager::create(ctx);

    auto const a = structure_of(0.25f);
    auto const first = manager.acquire(sv::quadric_data::of(a));
    manager.wait_for_settled();

    // The property the whole authoring model rests on: placing an unchanged batch every frame costs a hash lookup.
    // A second acquire of equal contents must be the SAME id, or a per-frame placement re-uploads the batch forever.
    auto const b = structure_of(0.25f);
    auto const second = manager.acquire(sv::quadric_data::of(b));
    CHECK(second == first);
    CHECK(manager.settling_count() == 0); // nothing was queued, so nothing is in flight

    // And a batch that really differs is a different resource, or a placement would hand back the wrong geometry.
    auto const c = structure_of(0.26f);
    auto const third = manager.acquire(sv::quadric_data::of(c));
    CHECK(third != first);
    CHECK(manager.settling_count() == 1);

    manager.wait_for_settled();
    CHECK(manager.get(third).state == sv::residency::complete);

    co_return;
}

ASYNC_INVOCABLE_TEST("sv - the resource manager drains quadric batches with everything else",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const set = structure_of(0.25f);
    auto const id = m.quadrics.acquire(sv::quadric_data::of(set));

    // A quadric batch is in flight the same way a mesh is, and the manager's own count covers both — which is what
    // makes "is anything still arriving" one question rather than one per resource kind.
    CHECK(m.quadrics.get(id).state == sv::residency::pending);
    CHECK(m.settling_count() >= 1);

    m.wait_for_pending_uploads();

    CHECK(m.quadrics.get(id).state == sv::residency::complete);
    CHECK(m.quadrics.get(id).blas != nullptr);
    CHECK(m.settling_count() == 0);

    co_return;
}

ASYNC_INVOCABLE_TEST("sv - a quadric batch can be a single primitive", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    auto manager = sv::quadric_manager::create(ctx);

    // One sphere is the smallest real batch, and a procedural BLAS over exactly one box is where an off-by-one in the
    // count or the stride shows up rather than being averaged away.
    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(2, 3, 4), 0.5f));

    auto const id = manager.acquire(sv::quadric_data::of(set));
    manager.wait_for_settled();

    CHECK(manager.get(id).state == sv::residency::complete);
    CHECK(manager.get(id).blas != nullptr);
    CHECK(manager.get(id).primitive_count == 1);
    CHECK(manager.get(id).aabbs.element_count() == 1);

    co_return;
}
