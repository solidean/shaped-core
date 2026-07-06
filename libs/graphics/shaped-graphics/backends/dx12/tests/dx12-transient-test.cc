#include "dx12-test-common.hh"

#include <nexus/test.hh>

// Transient buffers: per-epoch scratch sub-allocated by a bump allocator over one DEFAULT heap whose
// head resets each epoch (successive epochs alias the same storage). On WARP so they run headless on CI.
// See libs/graphics/shaped-graphics/docs/concepts/memory.md.
// NOTE (as in the transfer tests): without the barrier system, upload-then-download of the SAME buffer
// must span two command lists (state decays to COMMON at each submit).

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - transient buffer round-trips within its epoch")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;
    auto buf = c.transient.create_buffer(256, usage);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf.value(), 0, 256);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != cc::byte(i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - transient buffers in one epoch are independent")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;
    auto buf_a = c.transient.create_buffer(128, usage);
    auto buf_b = c.transient.create_buffer(128, usage);
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
    up.value()->upload.bytes_to_buffer(buf_a.value(), cc::span<cc::byte const>(src_a, 128));
    up.value()->upload.bytes_to_buffer(buf_b.value(), cc::span<cc::byte const>(src_b, 128));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future_a = down.value()->download.bytes_from_buffer(buf_a.value(), 0, 128);
    auto future_b = down.value()->download.bytes_from_buffer(buf_b.value(), 0, 128);
    c.submit_dx12_command_list(cc::move(down.value()));

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

TEST("sg dx12 - transient buffer reports expired once its epoch passes")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto buf = c.transient.create_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->is_valid()); // fresh: created in the current epoch
    CHECK(!buf.value()->is_expired());

    c.advance_epoch_and_wait_for_idle(); // its epoch has now passed -> auto-expired at advance
    CHECK(buf.value()->is_expired());    // using it now (transfer / binding) would be a hard error
    CHECK(!buf.value()->is_valid());
}

// Allocate one transient buffer per epoch for many epochs on a small budget. Each 256-byte buffer
// occupies a 64 KiB placement, so a 512 KiB budget holds only a handful — but the bump head resets
// every epoch, so successive epochs alias the same storage and every epoch's data still round-trips.
TEST("sg dx12 - transient buffer storage reused across many epochs")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);
    c.transient.set_budget(cc::isize(512) * 1024); // takes effect from the next epoch on (see set_budget)

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    for (int e = 0; e < 30; ++e)
    {
        auto buf = c.transient.create_buffer(256, usage);
        REQUIRE(buf.has_value());

        cc::byte src[256];
        for (int i = 0; i < 256; ++i)
            src[i] = cc::byte((i + e) & 0xFF);

        auto up = c.create_dx12_command_list();
        REQUIRE(up.has_value());
        up.value()->upload.bytes_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
        c.submit_dx12_command_list(cc::move(up.value()));

        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download.bytes_from_buffer(buf.value(), 0, 256);
        c.submit_dx12_command_list(cc::move(down.value()));

        auto const bytes = future.wait_get_bytes();
        REQUIRE(bytes.has_value());
        bool matches = true;
        for (int i = 0; i < 256; ++i)
            if (bytes.value()[i] != cc::byte((i + e) & 0xFF))
                matches = false;
        CHECK(matches);

        c.advance_epoch(2); // keep at most 2 epochs in flight → the bump head resets, aliasing storage
    }
}

// set_budget is deferred: it records a pending budget that the next advance_epoch applies by draining
// in-flight work and resizing the transient heap. Change the budget mid-run, with epochs in flight, and
// confirm transient buffers keep round-tripping across the resize (drain doesn't deadlock, heap is
// recreated at the new size, data survives).
TEST("sg dx12 - transient budget resize takes effect at the next epoch")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    auto const round_trip = [&](int tag)
    {
        auto buf = c.transient.create_buffer(256, usage);
        REQUIRE(buf.has_value());

        cc::byte src[256];
        for (int i = 0; i < 256; ++i)
            src[i] = cc::byte((i + tag) & 0xFF);

        auto up = c.create_dx12_command_list();
        REQUIRE(up.has_value());
        up.value()->upload.bytes_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
        c.submit_dx12_command_list(cc::move(up.value()));

        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download.bytes_from_buffer(buf.value(), 0, 256);
        c.submit_dx12_command_list(cc::move(down.value()));

        auto const bytes = future.wait_get_bytes();
        REQUIRE(bytes.has_value());
        bool matches = true;
        for (int i = 0; i < 256; ++i)
            if (bytes.value()[i] != cc::byte((i + tag) & 0xFF))
                matches = false;
        CHECK(matches);
    };

    // Epoch 0 uses the default budget (the heap is created here, before any set_budget lands).
    round_trip(0);

    // Request a small budget, then two more, keeping epochs in flight so the apply-time drain has work.
    c.transient.set_budget(cc::isize(512) * 1024);
    for (int e = 1; e <= 4; ++e)
    {
        c.advance_epoch(2); // first advance applies the pending 512 KiB budget (drains + resizes)
        round_trip(e);
    }

    // Grow the budget and confirm data still survives the resize.
    c.transient.set_budget(cc::isize(4) * 1024 * 1024);
    for (int e = 5; e <= 8; ++e)
    {
        c.advance_epoch(2);
        round_trip(e);
    }
}
