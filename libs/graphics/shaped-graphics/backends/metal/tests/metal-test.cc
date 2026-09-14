#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Bring-up tests for the metal backend: that a context comes up at all, what it reports about itself, and that the
// epoch timelines behave the way sg's contract says.
//
// Everything here builds its own context, because the context IS the subject — which is the rule
// libs/graphics/shaped-graphics/docs/testing.md sets for when that is allowed.

using namespace sg::backend::metal;

TEST("sg metal - a context comes up and describes its adapter")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    CHECK(ctx->backend() == sg::backend_kind::metal);
    CHECK(ctx->threading() == sg::thread_model::multi_threaded);
    CHECK(ctx->execution() == sg::execution_model::may_block);

    // Metal consumes compiled Metal libraries and nothing else.
    CHECK(ctx->accepts_shader_format(sg::shader_format::metal_lib));
    CHECK(!ctx->accepts_shader_format(sg::shader_format::spirv));
    CHECK(!ctx->accepts_shader_format(sg::shader_format::dxil));

    // Every device Metal hands out is real hardware; there is no software rasterizer to fall back to.
    CHECK(!ctx->adapter().is_software);
    CHECK(!ctx->adapter().name.empty());
}

TEST("sg metal - the ray-tracing answer is the backend's, not the device's")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Every Metal 4 device can ray trace, and this backend cannot yet.
    // Pinned so the gap reads as deliberate rather than as an omission — and so that closing it is a failing test here
    // rather than something nobody notices.
    // See libs/graphics/shaped-graphics/docs/writing-a-backend.md.
    CHECK(!ctx->supports(sg::feature::raytracing));

    // Metal has never had a geometry or tessellation stage, so these two are permanent rather than pending.
    CHECK(!ctx->supports(sg::feature::geometry_shader));
    CHECK(!ctx->supports(sg::feature::tessellation_shader));
}

TEST("sg metal - epochs advance and retire")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const start = ctx->current_epoch();
    CHECK(start == sg::epoch::first);

    // Nothing has been submitted, so nothing has completed — and the answer is `first - 1` rather than 0, which is what
    // lets an ordinary `<=` comparison work from the first frame.
    CHECK(ctx->completed_epoch() == sg::epoch(u64(sg::epoch::first) - 1));
    CHECK(ctx->in_flight_epoch_count() == 0);

    ctx->advance_epoch();
    CHECK(ctx->current_epoch() == sg::epoch(u64(start) + 1));

    // An empty epoch has no GPU work in it, so the signal lands as soon as the queue reaches it.
    ctx->block_until_idle();
    CHECK(ctx->completed_epoch() >= start);
    CHECK(ctx->in_flight_epoch_count() == 0);
}

TEST("sg metal - a command list can be opened and submitted empty")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const token = ctx->submit_command_list(ctx->create_command_list());
    CHECK(token != sg::submission_token::invalid);
    CHECK(token != sg::submission_token::not_submitted);

    ctx->block_until_idle();
    CHECK(ctx->is_submission_complete(token));
}

TEST("sg metal - a command list can be opened and dropped")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // A dropped list is never committed, so its allocator goes straight back rather than riding the epoch's GPU work.
    auto const before = ctx->current_epoch();
    ctx->drop_command_list(ctx->create_command_list());

    // Nothing was submitted, so dropping alone must not put an epoch in flight.
    CHECK(ctx->in_flight_epoch_count() == 0);

    ctx->advance_epoch();
    ctx->block_until_idle();

    CHECK(ctx->current_epoch() == sg::epoch(u64(before) + 1));
    CHECK(ctx->in_flight_epoch_count() == 0);
}

TEST("sg metal - an allocator is recycled across epochs")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The point of the epoch payload: an allocator submitted in epoch N is reset and handed back once N retires, so a
    // loop of frames creates a bounded number of them rather than one per list.
    for (auto i = 0; i < 8; ++i)
    {
        (void)ctx->submit_command_list(ctx->create_command_list());
        ctx->advance_epoch();
        ctx->block_until_epochs_in_flight(1);
    }

    ctx->block_until_idle();
    CHECK(ctx->in_flight_epoch_count() == 0);
}

TEST("sg metal - a heap's buffer requirements keep a bump allocator aligned")
{
    auto const ctx = test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const heap = ctx->persistent.create_memory_heap(4 * 1024 * 1024);

    // The invariant sg::context_transient_scope's bump allocator rests on: it advances its head by the reported size
    // and never re-aligns, so a reported size that is not a multiple of the reported alignment misaligns every
    // placement after the first.
    // Metal reports the two independently, so this is the backend's job rather than something the query guarantees.
    for (auto const size : {1, 3, 16, 17, 100, 256, 257, 4096, 65'537})
    {
        auto const reqs = heap->memory_requirements_for_buffer(size, sg::buffer_usage::copy_dst);
        CHECK(reqs.alignment_in_bytes > 0).context(cc::format("size {}", size));
        CHECK(reqs.size_in_bytes >= size).context(cc::format("size {}", size));
        CHECK(reqs.size_in_bytes % reqs.alignment_in_bytes == 0)
            .context(cc::format("size {} -> {} bytes at alignment {}", size, reqs.size_in_bytes, reqs.alignment_in_bytes));
    }
}
