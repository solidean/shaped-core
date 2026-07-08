#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/raw_buffer.hh>

// Runtime resizing of the transfer resources (ctx.upload.set_async_window_size / set_inline_budget and
// ctx.download.set_budget), on WARP. Each ring is created tiny, then grown, and a transfer that would not
// fit the original capacity is run to prove the resize took effect — plus a round-trip either side of the
// change to prove correctness is preserved. See upload.async / upload.inline / download.inline concept docs.

namespace
{
namespace dx12 = sg::backend::dx12;

// Fresh buffer, INLINE upload of `n` bytes (pattern (i+seed)), inline download, byte-exact check.
bool inline_round_trip(sg::context_handle const& ctx, cc::isize n, int seed)
{
    auto buf = ctx->persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    if (!buf)
        return false;

    cc::vector<cc::byte> src;
    src.reserve(n);
    for (cc::isize i = 0; i < n; ++i)
        src.push_back(cc::byte((i + seed) & 0xFF));

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buf, src);
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(down));

    auto bytes = ctx->wait_for(fut);
    if (!bytes.has_value() || bytes.value().size() != n)
        return false;
    for (cc::isize i = 0; i < n; ++i)
        if (bytes.value()[i] != cc::byte((i + seed) & 0xFF))
            return false;
    return true;
}

// Fresh buffer, ASYNC upload of `n` bytes (pattern (i+seed)), inline download, byte-exact check.
bool async_round_trip(sg::context_handle const& ctx, cc::isize n, int seed)
{
    auto buf = ctx->persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    if (!buf)
        return false;

    cc::vector<cc::byte> src;
    src.reserve(n);
    for (cc::isize i = 0; i < n; ++i)
        src.push_back(cc::byte((i + seed) & 0xFF));
    ctx->upload.bytes_to_buffer(buf, cc::make_pinned_data(cc::move(src)));

    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(down));

    auto bytes = ctx->wait_for(fut);
    if (!bytes.has_value() || bytes.value().size() != n)
        return false;
    for (cc::isize i = 0; i < n; ++i)
        if (bytes.value()[i] != cc::byte((i + seed) & 0xFF))
            return false;
    return true;
}
} // namespace

TEST("sg dx12 - async upload window resize preserves uploads")
{
    // A 1 KiB window (packs a larger upload across windows). Resizing changes only staging memory.
    auto ctx = sg::create_dx12_context({.use_warp = true, .async_upload_window_bytes = 1024});
    REQUIRE(ctx.has_value());

    CHECK(async_round_trip(ctx.value(), 4096, 1)); // spans several 1 KiB windows

    ctx.value()->upload.set_async_window_size(cc::isize(64) * 1024); // grow; actor adopts it before next upload
    CHECK(async_round_trip(ctx.value(), cc::isize(32) * 1024, 2));   // now fits one window

    ctx.value()->upload.set_async_window_size(2048); // shrink again
    CHECK(async_round_trip(ctx.value(), 8192, 3));   // packs across the smaller windows
}

TEST("sg dx12 - inline upload ring grows to fit a larger upload")
{
    // A 4 KiB upload ring: an upload larger than this asserts (a single upload cannot exceed capacity).
    auto ctx = sg::create_dx12_context({.use_warp = true, .upload_ring_bytes = 4096});
    REQUIRE(ctx.has_value());

    CHECK(inline_round_trip(ctx.value(), 2048, 1)); // fits the small ring

    ctx.value()->upload.set_inline_budget(cc::isize(128) * 1024); // grow the ring
    ctx.value()->advance_epoch(cc::nullopt);                      // applies the pending budget

    CHECK(inline_round_trip(ctx.value(), cc::isize(64) * 1024, 2)); // would not fit the original 4 KiB ring
}

TEST("sg dx12 - inline download ring grows to fit a larger readback")
{
    // A 4 KiB readback ring: a download larger than this asserts before the resize.
    auto ctx = sg::create_dx12_context({.use_warp = true, .download_ring_bytes = 4096});
    REQUIRE(ctx.has_value());

    CHECK(inline_round_trip(ctx.value(), 2048, 1)); // fits the small ring

    ctx.value()->download.set_budget(cc::isize(128) * 1024); // grow the readback ring
    ctx.value()->advance_epoch(cc::nullopt);                 // applies the pending budget (drains the actor)

    CHECK(inline_round_trip(ctx.value(), cc::isize(64) * 1024, 2)); // would not fit the original 4 KiB ring
}
