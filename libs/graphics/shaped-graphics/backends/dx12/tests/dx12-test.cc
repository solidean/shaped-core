#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// dx12 backend bring-up test. This is its own binary (shaped-graphics-dx12-test), built only where
// the dx12 backend builds (Windows), so it never needs an #ifdef guard. Backend-specific tests live
// here rather than in the backend-agnostic shaped-graphics-test.
//
// Two contexts are exercised:
//   - WARP (software adapter): always available on any Windows host, so it runs on headless CI.
//   - hardware (default): exercises a real GPU locally; skipped when no adapter is present (CI).
// The debug layer is requested but best-effort — create_dx12_context proceeds without it when the
// Graphics Tools feature isn't installed.

namespace
{
namespace dx12 = sg::backend::dx12;

// Drives the real dx12 paths against a live context via the backend-typed API (create_dx12_*), so
// there are no downcasts and we can inspect the concrete resources.
void exercise_context(dx12::dx12_context& ctx)
{
    // Command list: handed out already recording. Submitting consumes it (moved in).
    auto cmd = ctx.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd.value() != nullptr);
    ctx.submit_dx12_command_list(cc::move(cmd.value()));

    // Buffer with real GPU storage.
    auto buf = ctx.create_dx12_buffer(256, sg::buffer_usage::copy_dst);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf.value()->usage(), sg::buffer_usage::copy_dst));
    CHECK(buf.value()->_resource != nullptr);

    // Empty buffer: valid, and allocates no GPU resource.
    auto empty = ctx.create_dx12_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
    CHECK(empty.value()->_resource == nullptr);

    // read-write storage (UAV) usage takes the ALLOW_UNORDERED_ACCESS path.
    auto storage = ctx.create_dx12_buffer(1024, sg::buffer_usage::storage_read_write);
    REQUIRE(storage.has_value());
    CHECK(storage.value()->size_in_bytes() == 1024);

    // Explicit drop (backend-typed) also consumes the list.
    auto to_drop = ctx.create_dx12_command_list();
    REQUIRE(to_drop.has_value());
    ctx.drop_dx12_command_list(cc::move(to_drop.value()));

    // The abstract sg::context API forwards to the same places: create a buffer, and create + drop a
    // command list, all through the base type.
    auto& base = static_cast<sg::context&>(ctx);

    auto via_base = base.create_buffer(64, sg::buffer_usage::vertex);
    REQUIRE(via_base.has_value());
    CHECK(via_base.value()->size_in_bytes() == 64);

    auto base_cmd = base.create_command_list();
    REQUIRE(base_cmd.has_value());
    base.drop_command_list(cc::move(base_cmd.value()));
}
} // namespace

TEST("sg dx12 - warp context")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    REQUIRE(ctx.has_value());
    CHECK(ctx.value()->backend() == sg::backend_kind::dx12);
    CHECK(ctx.value()->threading() == sg::thread_model::multi_threaded);
    exercise_context(static_cast<dx12::dx12_context&>(*ctx.value()));
}

TEST("sg dx12 - hardware context")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true});
    if (ctx.has_error())
        return; // no D3D12-capable hardware adapter (e.g. headless CI); WARP test covers the path.

    CHECK(ctx.value()->backend() == sg::backend_kind::dx12);
    exercise_context(static_cast<dx12::dx12_context&>(*ctx.value()));
}

// --- Epoch system --------------------------------------------------------------------------------
// All epoch tests run on WARP so they exercise a live epoch fence on headless CI. Note: the "a list
// must be submitted/dropped in the epoch it was opened in" and "no open lists at advance" contracts
// are CC_ASSERT-enforced; we don't test the abort paths (nexus has no death test).

namespace
{
// Fresh WARP context for an epoch test, or nullptr on the rare host without WARP.
sg::context_handle make_warp_context()
{
    auto ctx = sg::create_dx12_context({.use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sg dx12 - epoch advance and retire")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    CHECK(c.current_epoch() == sg::epoch::first);
    // Nothing has finished yet, so the completed epoch is first-1.
    CHECK(cc::u64(c.completed_epoch()) == cc::u64(sg::epoch::first) - 1);

    c.advance_epoch_and_wait_for_idle();
    CHECK(c.current_epoch() == sg::epoch(cc::u64(sg::epoch::first) + 1));
    CHECK(cc::u64(c.completed_epoch()) >= cc::u64(sg::epoch::first)); // the first epoch is now done
}

TEST("sg dx12 - deferred deletion runs finalizers only after the owning epoch retires")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    bool finalized = false;
    {
        auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_dst);
        REQUIRE(buf.has_value());
        buf.value()->add_finalizer([&finalized] { finalized = true; });
    } // last handle dropped here → deferred deletion staged in the current epoch

    // The owning epoch has not advanced/retired yet, so the resource is still (potentially) in use.
    CHECK(!finalized);

    c.advance_epoch_and_wait_for_idle(); // closes + drains the epoch the buffer died in
    CHECK(finalized);
}

TEST("sg dx12 - command allocators are recycled across epochs")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const free_count
        = [&] { return c._allocators.lock([](dx12::dx12_allocator_pool& p) { return p.free.size(); }); };

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    c.submit_dx12_command_list(cc::move(cmd.value()));
    CHECK(free_count() == 0); // still in flight — captured by the current epoch

    c.advance_epoch_and_wait_for_idle();
    CHECK(free_count() == 1); // reset and returned to the free pool on retire

    // The next list reuses the pooled allocator rather than creating a new one.
    auto cmd2 = c.create_dx12_command_list();
    REQUIRE(cmd2.has_value());
    CHECK(free_count() == 0);
    c.submit_dx12_command_list(cc::move(cmd2.value()));
    c.advance_epoch_and_wait_for_idle();
    CHECK(free_count() == 1);
}

TEST("sg dx12 - submission token reports completion")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    auto const token = c.submit_dx12_command_list(cc::move(cmd.value()));

    c.advance_epoch_and_wait_for_idle(); // forces the GPU to catch up
    CHECK(c.is_submission_complete(token));
    CHECK(!c.is_submission_complete(sg::submission_token::not_submitted));
}

TEST("sg dx12 - throttle bounds epochs in flight")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // Allow at most one prior epoch in flight; after several advances the FIFO stays bounded.
    for (int i = 0; i < 5; ++i)
        c.advance_epoch(1);

    auto const in_flight = c._epoch_state.lock([](dx12::dx12_epoch_state& s) { return s.in_flight.size(); });
    CHECK(in_flight <= 1);
}
