#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/raw_buffer.hh>

// Backend-internal test: an inline transfer that would straddle the ring seam is split into two
// contiguous copies rather than wasting the tail and restarting at 0. We peel the abstraction (the
// legitimate escape hatch — see libs/graphics/shaped-graphics/docs/testing.md): create a tiny ring, drain
// it, park the logical cursor
// a few bytes before the physical seam via the system's debug_set_cursor hook, run a transfer bigger
// than that gap, and assert the cursor advanced by *exactly* the transfer size — a wasted tail would add
// the skipped gap on top. A byte-exact round-trip proves the seam-straddling copy is correct.
//
// Covers the seam-split in dx12_upload_inline / dx12_download_inline (concept docs
// libs/graphics/shaped-graphics/docs/concepts/upload.inline.md /
// libs/graphics/shaped-graphics/docs/concepts/download.inline.md).

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr cc::isize ring_bytes = 256; // tiny ring so a small transfer can be made to wrap
constexpr cc::isize seam_gap = 64;    // bytes left before the seam when we park the cursor
constexpr cc::isize xfer_bytes = 128; // > seam_gap, so the transfer must cross the seam
} // namespace

TEST("sg dx12 - inline upload splits across the ring seam")
{
    auto ctx_r = sg::create_dx12_context({.use_warp = true, .upload_ring_bytes = ring_bytes});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();
    auto& c = static_cast<dx12::dx12_context&>(*ctx);

    auto buf = ctx->persistent.create_raw_buffer(xfer_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    // Drain, then park the upload ring cursor `seam_gap` bytes before the physical seam.
    ctx->advance_epoch_and_wait_for_idle();
    c._upload_inline.debug_set_cursor(cc::u64(ring_bytes - seam_gap));

    auto const before = c._upload_inline.debug_cursor();
    REQUIRE(before.capacity == ring_bytes);
    REQUIRE(cc::isize(before.next_pos % cc::u64(ring_bytes)) + xfer_bytes > ring_bytes); // setup forces a wrap

    // Record a wrapping upload of a known pattern.
    cc::vector<cc::byte> src;
    src.reserve(xfer_bytes);
    for (cc::isize i = 0; i < xfer_bytes; ++i)
        src.push_back(cc::byte((i * 7 + 3) & 0xFF));

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buf, src);
    ctx->submit_command_list(cc::move(up));

    // Split, not tail-waste: the cursor advanced by exactly the transfer size (a wasted tail would add
    // the skipped `seam_gap` bytes on top).
    auto const after = c._upload_inline.debug_cursor();
    CHECK(after.next_pos - before.next_pos == cc::u64(xfer_bytes));

    // The seam-straddling copy is byte-correct.
    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_buffer(buf, 0, xfer_bytes);
    ctx->submit_command_list(cc::move(down));

    auto bytes = ctx->wait_for(fut);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == xfer_bytes);
    for (cc::isize i = 0; i < xfer_bytes; ++i)
        CHECK(bytes.value()[i] == cc::byte((i * 7 + 3) & 0xFF));
}

TEST("sg dx12 - inline download splits across the ring seam")
{
    auto ctx_r = sg::create_dx12_context({.use_warp = true, .download_ring_bytes = ring_bytes});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();
    auto& c = static_cast<dx12::dx12_context&>(*ctx);

    auto buf = ctx->persistent.create_raw_buffer(xfer_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    // Seed the buffer with a known pattern (default upload ring, no wrap here).
    cc::vector<cc::byte> src;
    src.reserve(xfer_bytes);
    for (cc::isize i = 0; i < xfer_bytes; ++i)
        src.push_back(cc::byte((i * 5 + 1) & 0xFF));
    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buf, src);
    ctx->submit_command_list(cc::move(up));

    // Drain (so the readback ring is empty), then park its cursor `seam_gap` bytes before the seam.
    ctx->advance_epoch_and_wait_for_idle();
    c._download_inline.debug_set_cursor(cc::u64(ring_bytes - seam_gap));

    auto const before = c._download_inline.debug_cursor();
    REQUIRE(before.capacity == ring_bytes);
    REQUIRE(cc::isize(before.next_pos % cc::u64(ring_bytes)) + xfer_bytes > ring_bytes); // setup forces a wrap

    // Record a wrapping readback. Reservation happens at record time, so the cursor is observable now.
    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_buffer(buf, 0, xfer_bytes);

    auto const after = c._download_inline.debug_cursor();
    CHECK(after.next_pos - before.next_pos == cc::u64(xfer_bytes)); // split, not tail-waste

    ctx->submit_command_list(cc::move(down));

    auto bytes = ctx->wait_for(fut);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == xfer_bytes);
    for (cc::isize i = 0; i < xfer_bytes; ++i)
        CHECK(bytes.value()[i] == cc::byte((i * 5 + 1) & 0xFF));
}
