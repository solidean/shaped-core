#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

using namespace cc::primitive_defines;

// Runtime resizing of the transfer resources: ctx.upload.set_async_window_size / set_inline_budget and ctx.download.set_budget.
// Each ring is created tiny, then grown, and a transfer that would not fit the original capacity proves the resize took effect.
// A round-trip either side of the change proves correctness is preserved.
// See the upload.async / upload.inline / download.inline concept docs.

namespace
{
namespace dx12 = sg::backend::dx12;

// Fresh buffer, INLINE upload of `n` bytes (pattern (i+seed)), inline download, byte-exact check.
cc::shared_async<bool> inline_round_trip(sg::context_handle const& ctx, isize n, int seed)
{
    auto buf = ctx->persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    if (!buf)
        co_return false;

    cc::vector<byte> src;
    src.reserve(n);
    for (isize i = 0; i < n; ++i)
        src.push_back(byte((i + seed) & 0xFF));

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buf, src);
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(down));

    co_await ctx->idle_completion();
    auto bytes = fut.try_get_bytes();
    if (!bytes.has_value() || bytes.value().size() != n)
        co_return false;
    for (isize i = 0; i < n; ++i)
        if (bytes.value()[i] != byte((i + seed) & 0xFF))
            co_return false;
    co_return true;
}

// Fresh buffer, ASYNC upload of `n` bytes (pattern (i+seed)), inline download, byte-exact check.
cc::shared_async<bool> async_round_trip(sg::context_handle const& ctx, isize n, int seed)
{
    auto buf = ctx->persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    if (!buf)
        co_return false;

    cc::vector<byte> src;
    src.reserve(n);
    for (isize i = 0; i < n; ++i)
        src.push_back(byte((i + seed) & 0xFF));
    ctx->upload.bytes_to_buffer(buf, cc::make_pinned_data(cc::move(src)));

    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(down));

    co_await ctx->idle_completion();
    auto bytes = fut.try_get_bytes();
    if (!bytes.has_value() || bytes.value().size() != n)
        co_return false;
    for (isize i = 0; i < n; ++i)
        if (bytes.value()[i] != byte((i + seed) & 0xFF))
            co_return false;
    co_return true;
}
} // namespace

ASYNC_TEST("sg dx12 - async upload window resize preserves uploads")
{
    // A 1 KiB window (packs a larger upload across windows). Resizing changes only staging memory.
    auto ctx = dx12::make_test_context({.async_upload_window_bytes = 1024});
    REQUIRE(ctx.has_value());

    auto const async_round_trip_ok_1 = co_await async_round_trip(ctx.value(), 4096, 1);
    CHECK(async_round_trip_ok_1); // spans several 1 KiB windows

    ctx.value()->upload.set_async_window_size(isize(64) * 1024); // grow; actor adopts it before next upload
    auto const async_round_trip_ok_2 = co_await async_round_trip(ctx.value(), isize(32) * 1024, 2);
    CHECK(async_round_trip_ok_2); // now fits one window

    ctx.value()->upload.set_async_window_size(2048); // shrink again
    auto const async_round_trip_ok_3 = co_await async_round_trip(ctx.value(), 8192, 3);
    CHECK(async_round_trip_ok_3); // packs across the smaller windows
}

ASYNC_TEST("sg dx12 - inline upload ring grows to fit a larger upload")
{
    // A 4 KiB upload ring: an upload larger than this asserts (a single upload cannot exceed capacity).
    auto ctx = dx12::make_test_context({.upload_ring_bytes = 4096});
    REQUIRE(ctx.has_value());

    auto const inline_round_trip_ok_1 = co_await inline_round_trip(ctx.value(), 2048, 1);
    CHECK(inline_round_trip_ok_1); // fits the small ring

    ctx.value()->upload.set_inline_budget(isize(128) * 1024); // grow the ring
    ctx.value()->advance_epoch();                             // applies the pending budget

    auto const inline_round_trip_ok_2 = co_await inline_round_trip(ctx.value(), isize(64) * 1024, 2);
    CHECK(inline_round_trip_ok_2); // would not fit the original 4 KiB ring
}

ASYNC_TEST("sg dx12 - inline download ring grows to fit a larger readback")
{
    // A 4 KiB readback ring: a download larger than this asserts before the resize.
    auto ctx = dx12::make_test_context({.download_ring_bytes = 4096});
    REQUIRE(ctx.has_value());

    auto const inline_round_trip_ok_3 = co_await inline_round_trip(ctx.value(), 2048, 1);
    CHECK(inline_round_trip_ok_3); // fits the small ring

    ctx.value()->download.set_budget(isize(128) * 1024); // grow the readback ring
    ctx.value()->advance_epoch();                        // applies the pending budget (drains the actor)

    auto const inline_round_trip_ok_4 = co_await inline_round_trip(ctx.value(), isize(64) * 1024, 2);
    CHECK(inline_round_trip_ok_4); // would not fit the original 4 KiB ring
}
