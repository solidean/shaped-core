#include "dx12-test-common.hh"

#include <nexus/test.hh>

// Inline buffer transfer: upload / download over the inline UPLOAD / READBACK ring buffers, on WARP
// so they run on headless CI. See libs/graphics/shaped-graphics/docs/concepts/upload.inline.md and
// libs/graphics/shaped-graphics/docs/concepts/download.inline.md.
// NOTE: these tests split upload and download across separate command lists. Same-list upload-then-
// download of one buffer is also correct now (dx12_command_list::record_transfer_barrier orders it) —
// the backend-agnostic tests (tests/transfer/) cover that single-list path.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - buffer upload then download round-trips")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    c.submit_command_list(cc::move(up));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 0, 256);
    c.submit_command_list(cc::move(down));

    // Ready after the submitted list finishes on the GPU — no advance_epoch needed.
    auto const bytes = c.wait_for(future);
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
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(cc::isize(4) * sizeof(int),
                                              sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    int const in[4] = {5, 6, 7, 8};
    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.data_to_buffer(buf, cc::span<int const>(in, 4));
    c.submit_command_list(cc::move(up));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<int>(buf, 0, 4);
    c.submit_command_list(cc::move(down));

    auto const data = c.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 5);
    CHECK(data.value()[3] == 8);
}

TEST("sg dx12 - empty transfers")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(16, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_buffer(buf, cc::span<cc::byte const>()); // no-op
    auto future = cmd->download.bytes_from_buffer(buf, 0, 0);
    CHECK(future.is_valid());
    CHECK(future.is_ready()); // zero-size read is immediately ready
    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().empty());
    c.submit_command_list(cc::move(cmd));
}

TEST("sg dx12 - partial download with offset")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    c.submit_command_list(cc::move(up));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 64, 64);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
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
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(16, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::byte first[16];
    cc::byte second[16];
    for (int i = 0; i < 16; ++i)
    {
        first[i] = cc::byte(0xAA);
        second[i] = cc::byte(0xBB);
    }

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(first, 16));
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(second, 16)); // overwrites
    c.submit_command_list(cc::move(up));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 0, 16);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    bool all_second = true;
    for (int i = 0; i < 16; ++i)
        if (bytes.value()[i] != cc::byte(0xBB))
            all_second = false;
    CHECK(all_second);
}

TEST("sg dx12 - dropping a download future is safe and reclaims ring space")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    c.submit_command_list(cc::move(up));

    {
        auto down = c.create_command_list();
        REQUIRE(down != nullptr);
        auto future = down->download.bytes_from_buffer(buf, 0, 256);
        c.submit_command_list(cc::move(down));
        // future dropped here without waiting → the actor cancels the memcpy but still frees the space
    }

    c.advance_epoch_and_wait_for_idle(); // let the GPU + actor settle

    auto down2 = c.create_command_list();
    REQUIRE(down2 != nullptr);
    auto future = down2->download.bytes_from_buffer(buf, 0, 256);
    c.submit_command_list(cc::move(down2));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().size() == 256);
    CHECK(bytes.value()[100] == cc::byte(100));
}

TEST("sg dx12 - inline transfer reused across epochs")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    for (int e = 0; e < 4; ++e)
    {
        cc::byte src[1024];
        for (int i = 0; i < 1024; ++i)
            src[i] = cc::byte((i + e) & 0xFF);

        auto up = c.create_command_list();
        REQUIRE(up != nullptr);
        up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 1024));
        c.submit_command_list(cc::move(up));

        auto down = c.create_command_list();
        REQUIRE(down != nullptr);
        auto future = down->download.bytes_from_buffer(buf, 0, 1024);
        c.submit_command_list(cc::move(down));

        auto const bytes = c.wait_for(future);
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
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf_a = c.persistent.create_raw_buffer(128, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto buf_b = c.persistent.create_raw_buffer(128, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf_a != nullptr);
    REQUIRE(buf_b != nullptr);

    cc::byte src_a[128];
    cc::byte src_b[128];
    for (int i = 0; i < 128; ++i)
    {
        src_a[i] = cc::byte(0xA0 + (i & 0xF));
        src_b[i] = cc::byte(0xB0 + (i & 0xF));
    }

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf_a, cc::span<cc::byte const>(src_a, 128));
    up->upload.bytes_to_buffer(buf_b, cc::span<cc::byte const>(src_b, 128));
    c.submit_command_list(cc::move(up));

    // Two lists open at once; A reserves its ring window first, then B reserves the next window.
    auto list_a = c.create_command_list();
    auto list_b = c.create_command_list();
    REQUIRE(list_a != nullptr);
    REQUIRE(list_b != nullptr);
    auto future_a = list_a->download.bytes_from_buffer(buf_a, 0, 128);
    auto future_b = list_b->download.bytes_from_buffer(buf_b, 0, 128);

    // Submit B before A: submission (and thus copy) order is the reverse of allocation order.
    c.submit_command_list(cc::move(list_b));
    c.submit_command_list(cc::move(list_a));

    auto const bytes_a = c.wait_for(future_a);
    auto const bytes_b = c.wait_for(future_b);
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
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    c.submit_command_list(cc::move(up));

    // Record a download, then drop the list without submitting → the future is cancelled.
    auto dropped = c.create_command_list();
    REQUIRE(dropped != nullptr);
    auto cancelled = dropped->download.bytes_from_buffer(buf, 0, 256);
    c.drop_command_list(cc::move(dropped));

    CHECK(!cancelled.is_ready());
    CHECK(!c.wait_for(cancelled).has_value()); // cancelled: fails, does not block

    c.advance_epoch_and_wait_for_idle(); // reclaims the dropped list's ring span with its epoch

    // The ring is free again: a fresh download round-trips.
    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 0, 256);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[100] == cc::byte(100));
}
