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
    auto buf = ctx.create_dx12_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf.value()->usage(), sg::buffer_usage::copy_dst));
    CHECK(buf.value()->_resource != nullptr);

    // Empty buffer: valid, and allocates no GPU resource.
    auto empty = ctx.create_dx12_buffer(0, sg::buffer_usage::none, sg::allocation_info{});
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
    CHECK(empty.value()->_resource == nullptr);

    // read-write storage (UAV) usage takes the ALLOW_UNORDERED_ACCESS path.
    auto storage = ctx.create_dx12_buffer(1024, sg::buffer_usage::readwrite_buffer, sg::allocation_info{});
    REQUIRE(storage.has_value());
    CHECK(storage.value()->size_in_bytes() == 1024);

    // Explicit drop (backend-typed) also consumes the list.
    auto to_drop = ctx.create_dx12_command_list();
    REQUIRE(to_drop.has_value());
    ctx.drop_dx12_command_list(cc::move(to_drop.value()));

    // The abstract sg::context API forwards to the same places: create a buffer, and create + drop a
    // command list, all through the base type.
    auto& base = static_cast<sg::context&>(ctx);

    auto via_base = base.persistent.create_buffer(64, sg::buffer_usage::vertex_buffer);
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
        auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
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

// --- Inline buffer transfer --------------------------------------------------------------------
// Upload / download over the inline UPLOAD / READBACK ring buffers, on WARP so they run on headless
// CI. NOTE: without the barrier system, uploading and then downloading the SAME buffer must span two
// command lists (buffer state decays to COMMON at each submit); these tests follow that pattern.

TEST("sg dx12 - buffer upload then download round-trips")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download_from_buffer(buf.value(), 0, 256);
    c.submit_dx12_command_list(cc::move(down.value()));

    // Ready after the submitted list finishes on the GPU — no advance_epoch needed.
    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != cc::byte(i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - typed upload/download convenience")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(cc::isize(4) * sizeof(int), sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    int const in[4] = {5, 6, 7, 8};
    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_data_to_buffer(buf.value(), cc::span<int const>(in, 4));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download_data_from_buffer<int>(buf.value(), 0, 4);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const data = future.wait_get_data();
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 5);
    CHECK(data.value()[3] == 8);
}

TEST("sg dx12 - empty transfers")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(16, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>()); // no-op
    auto future = cmd.value()->download_from_buffer(buf.value(), 0, 0);
    CHECK(future.is_valid());
    CHECK(future.is_ready()); // zero-size read is immediately ready
    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().empty());
    c.submit_dx12_command_list(cc::move(cmd.value()));
}

TEST("sg dx12 - partial download with offset")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download_from_buffer(buf.value(), 64, 64);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 64);
    bool matches = true;
    for (int i = 0; i < 64; ++i)
        if (bytes.value()[i] != cc::byte(64 + i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - multiple uploads in one list, last writer wins")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(16, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    cc::byte first[16];
    cc::byte second[16];
    for (int i = 0; i < 16; ++i)
    {
        first[i] = cc::byte(0xAA);
        second[i] = cc::byte(0xBB);
    }

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(first, 16));
    up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(second, 16)); // overwrites
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download_from_buffer(buf.value(), 0, 16);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    bool all_second = true;
    for (int i = 0; i < 16; ++i)
        if (bytes.value()[i] != cc::byte(0xBB))
            all_second = false;
    CHECK(all_second);
}

TEST("sg dx12 - dropping a download future is safe and reclaims ring space")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    {
        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download_from_buffer(buf.value(), 0, 256);
        c.submit_dx12_command_list(cc::move(down.value()));
        // future dropped here without waiting → the actor cancels the memcpy but still frees the space
    }

    c.advance_epoch_and_wait_for_idle(); // let the GPU + actor settle

    auto down2 = c.create_dx12_command_list();
    REQUIRE(down2.has_value());
    auto future = down2.value()->download_from_buffer(buf.value(), 0, 256);
    c.submit_dx12_command_list(cc::move(down2.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().size() == 256);
    CHECK(bytes.value()[100] == cc::byte(100));
}

TEST("sg dx12 - inline transfer reused across epochs")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    for (int e = 0; e < 4; ++e)
    {
        cc::byte src[1024];
        for (int i = 0; i < 1024; ++i)
            src[i] = cc::byte((i + e) & 0xFF);

        auto up = c.create_dx12_command_list();
        REQUIRE(up.has_value());
        up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(src, 1024));
        c.submit_dx12_command_list(cc::move(up.value()));

        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download_from_buffer(buf.value(), 0, 1024);
        c.submit_dx12_command_list(cc::move(down.value()));

        auto const bytes = future.wait_get_bytes();
        REQUIRE(bytes.has_value());
        bool matches = true;
        for (int i = 0; i < 1024; ++i)
            if (bytes.value()[i] != cc::byte((i + e) & 0xFF))
                matches = false;
        CHECK(matches);

        c.advance_epoch(2);
    }
}

// Two lists record downloads concurrently (interleaved ring reservations) but submit in the OPPOSITE
// order. The actor copies in submission order — which no longer matches ring-allocation order — so a
// per-submission free watermark would reclaim the first-allocated window while the other list still
// holds it. Epoch-granular reclaim must keep both windows pinned; both futures must read back intact.
TEST("sg dx12 - interleaved downloads submitted out of allocation order")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf_a = c.create_dx12_buffer(128, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    auto buf_b = c.create_dx12_buffer(128, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf_a.has_value());
    REQUIRE(buf_b.has_value());

    cc::byte src_a[128];
    cc::byte src_b[128];
    for (int i = 0; i < 128; ++i)
    {
        src_a[i] = cc::byte(0xA0 + (i & 0xF));
        src_b[i] = cc::byte(0xB0 + (i & 0xF));
    }

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_to_buffer(buf_a.value(), cc::span<cc::byte const>(src_a, 128));
    up.value()->upload_to_buffer(buf_b.value(), cc::span<cc::byte const>(src_b, 128));
    c.submit_dx12_command_list(cc::move(up.value()));

    // Two lists open at once; A reserves its ring window first, then B reserves the next window.
    auto list_a = c.create_dx12_command_list();
    auto list_b = c.create_dx12_command_list();
    REQUIRE(list_a.has_value());
    REQUIRE(list_b.has_value());
    auto future_a = list_a.value()->download_from_buffer(buf_a.value(), 0, 128);
    auto future_b = list_b.value()->download_from_buffer(buf_b.value(), 0, 128);

    // Submit B before A: submission (and thus copy) order is the reverse of allocation order.
    c.submit_dx12_command_list(cc::move(list_b.value()));
    c.submit_dx12_command_list(cc::move(list_a.value()));

    auto const bytes_a = future_a.wait_get_bytes();
    auto const bytes_b = future_b.wait_get_bytes();
    REQUIRE(bytes_a.has_value());
    REQUIRE(bytes_b.has_value());
    bool ok_a = true;
    bool ok_b = true;
    for (int i = 0; i < 128; ++i)
    {
        if (bytes_a.value()[i] != cc::byte(0xA0 + (i & 0xF)))
            ok_a = false;
        if (bytes_b.value()[i] != cc::byte(0xB0 + (i & 0xF)))
            ok_b = false;
    }
    CHECK(ok_a);
    CHECK(ok_b);
}

// Dropping (never submitting) a list with a pending download cancels its future: it never becomes
// ready, wait fails instead of blocking forever, and the ring space it reserved is reclaimed with the
// epoch so later downloads still succeed.
TEST("sg dx12 - dropping a recording list cancels its downloads")
{
    auto handle = make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    // Record a download, then drop the list without submitting → the future is cancelled.
    auto dropped = c.create_dx12_command_list();
    REQUIRE(dropped.has_value());
    auto cancelled = dropped.value()->download_from_buffer(buf.value(), 0, 256);
    c.drop_dx12_command_list(cc::move(dropped.value()));

    CHECK(!cancelled.is_ready());
    CHECK(!cancelled.wait_get_bytes().has_value()); // cancelled: fails, does not block

    c.advance_epoch_and_wait_for_idle(); // reclaims the dropped list's ring span with its epoch

    // The ring is free again: a fresh download round-trips.
    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download_from_buffer(buf.value(), 0, 256);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[100] == cc::byte(100));
}
