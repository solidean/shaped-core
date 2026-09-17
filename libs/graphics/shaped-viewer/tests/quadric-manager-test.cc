#include "viewer_test_env.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/context.hh> // sv::background_work
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

ASYNC_INVOCABLE_TEST("sv - an idle quadric batch is evicted like any other resource", (sg::context_handle const& ctx_h))
{
    // `gpu_resource_manager::advance_to` drives every pool's frame boundary, and the quadric pool was left out of it.
    // Nothing about that is visible in a short-lived scene: the pool stays at epoch 0, so the idle timeout never fires
    // and the byte budget treats every entry as this frame's working set.
    // A configured `quadrics.budget` therefore did nothing at all.
    auto& ctx = *ctx_h;

    auto resources = sv::gpu_resource_manager::create(ctx, {.quadrics = {.budget = {.max_idle_epochs = 0}}});

    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 0.5f));

    auto const id = resources.quadrics.acquire(sv::quadric_data::of(set));
    REQUIRE(resources.quadrics.get_ptr(id) != nullptr);

    // Landed before the boundaries, so what is being tested is the IDLE timeout rather than a cancelled upload.
    resources.wait_for_pending_uploads();

    // Two boundaries without re-acquiring: at `max_idle_epochs = 0` the first one that does not touch it is enough.
    resources.advance_to(sg::epoch(u64(ctx.current_epoch()) + 1));
    resources.advance_to(sg::epoch(u64(ctx.current_epoch()) + 2));

    CHECK(resources.quadrics.get_ptr(id) == nullptr);

    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a refilled quadric set re-acquires rather than keeping its first geometry",
                     (sg::context_handle const& ctx_h))
{
    // The slot is a cache keyed on CONTENT, not merely validated for liveness.
    // `clear()` plus a refill is the per-frame pattern the type documents, and it changes the primitives without
    // touching the slot — so a slot keyed on liveness alone would keep drawing whatever the set held when it was
    // first placed.
    auto& ctx = *ctx_h;
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 0.5f));

    auto const& first = resources.create_quadric_set(set);
    auto const first_id = first.geometry;
    CHECK(first.primitive_count == 1);

    set.clear();
    set.add_sphere(tg::sphere3f(tg::pos3f(1, 0, 0), 0.25f));
    set.add_sphere(tg::sphere3f(tg::pos3f(2, 0, 0), 0.25f));

    auto const& second = resources.create_quadric_set(set);
    CHECK(second.geometry != first_id);
    CHECK(second.primitive_count == 2);

    // An UNCHANGED set still takes the fast path, which is the property the content key must not cost.
    auto const& again = resources.create_quadric_set(set);
    CHECK(again.geometry == second.geometry);

    // A copy carries the original's slot, and its own contents decide what it resolves to.
    auto copy = set;
    copy.add_sphere(tg::sphere3f(tg::pos3f(3, 0, 0), 0.25f));
    auto const& copied = resources.create_quadric_set(copy);
    CHECK(copied.geometry != second.geometry);
    CHECK(copied.primitive_count == 3);

    resources.wait_for_pending_uploads();
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a reassigned mesh re-acquires rather than keeping its first geometry",
                     (sg::context_handle const& ctx_h))
{
    // The same property on `sv::mesh`, whose `geometry`, `attributes` and `textures` are all public and mutable.
    auto& ctx = *ctx_h;
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const tri = [](float x)
    {
        return sv::triangle_geometry::create_from_positions(
            cc::vector<tg::pos3f>{tg::pos3f(x, 0, 0), tg::pos3f(x + 1, 0, 0), tg::pos3f(x, 1, 0)});
    };

    auto mesh = sv::mesh{.name = "m", .geometry = tri(0.0f)};

    auto const& first = resources.create_mesh(mesh);
    auto const first_id = first.geometry;

    mesh.geometry = tri(5.0f);

    auto const& second = resources.create_mesh(mesh);
    CHECK(second.geometry != first_id);

    // Unchanged again takes the fast path.
    auto const& again = resources.create_mesh(mesh);
    CHECK(again.geometry == second.geometry);

    // And an attribute is part of the key, since the binding copies its name, format and frequency across.
    mesh.attributes.push_back(sv::mesh_attribute::create_value("base_color", tg::vec3f(1, 0, 0)));
    auto const& with_attribute = resources.create_mesh(mesh);
    CHECK(with_attribute.attributes.size() == 1);

    resources.wait_for_pending_uploads();
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a pending quadric batch names the placeholder's positions", (sg::context_handle const& ctx_h))
{
    // A pending batch is traced as the placeholder CUBE through the triangle fallback, and that hit group reads the
    // triangle's three corner positions back out of `inst.vertices` to recompute the geometric normal.
    // Naming the index stand-in there — a three-element u32 buffer — hands it twelve bytes to read up to 36 positions
    // out of, which is the mesh overload's own documented reason for naming `placeholder_vertices` instead.
    //
    // Checked without a trace on purpose: a trace would race both the upload and the stand-in's compile.
    auto& ctx = *ctx_h;
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 0.5f));

    // A batch stays pending until something drains it, and nothing here does.
    auto const item = resources.acquire_scene_item(set);
    auto const* const record = resources.quadrics.get_ptr(item.quadrics);
    REQUIRE(record != nullptr);
    REQUIRE(record->state != sv::residency::complete);

    auto cmd = ctx.create_command_list();
    auto const described = resources.describe_instance(*cmd, item.quadrics, item.instance);

    // `acquire_buffer` hands back the same index for the same view within one epoch, so this compares indices.
    auto const expected = u32(resources.acquire_buffer(resources.meshes.placeholder_vertices().raw()->as_raw_readonly()));
    CHECK(described.vertices == expected);

    // And it is NOT the index stand-in, which is what it used to name.
    auto const stand_in = u32(resources.acquire_buffer(resources.meshes.index_stand_in().raw()->as_raw_readonly()));
    CHECK(described.vertices != stand_in);
    CHECK(described.indices == stand_in); // a quadric indexes nothing, so that field still names the stand-in

    ctx.drop_command_list(cc::move(cmd));

    resources.wait_for_pending_uploads();
    co_await cc::async_settled(sv::background_work(ctx));
}
