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
// Each ring is shrunk on the shared context, then grown, and a transfer that would not fit the small capacity proves the resize took effect.
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

    auto const bytes = co_await fut.bytes();
    if (bytes.size() != n)
        co_return false;
    for (isize i = 0; i < n; ++i)
        if (bytes[i] != byte((i + seed) & 0xFF))
            co_return false;
    co_return true;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg dx12 - async upload window resize preserves uploads", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    // A 1 KiB window (packs a larger upload across windows). Resizing changes only staging memory.
    auto const sizes = dx12::scoped_transfer_sizes(*handle, {.async_upload_window_bytes = 1024});

    CHECK(co_await async_round_trip(handle, 4096, 1)); // spans several 1 KiB windows
    dx12::require_async_upload_window(*handle, 1024);  // the guard's size applied, so it really spanned them

    handle->upload.set_async_window_size(isize(64) * 1024);        // grow; actor adopts it before next upload
    CHECK(co_await async_round_trip(handle, isize(32) * 1024, 2)); // now fits one window

    handle->upload.set_async_window_size(2048);        // shrink again
    CHECK(co_await async_round_trip(handle, 8192, 3)); // packs across the smaller windows
}

ASYNC_INVOCABLE_TEST("sg dx12 - inline upload ring grows to fit a larger upload",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    // A 4 KiB upload ring: an upload larger than this asserts (a single upload cannot exceed capacity).
    auto const sizes = dx12::scoped_transfer_sizes(*handle, {.upload_ring_bytes = 4096});
    REQUIRE(handle->_upload_inline.debug_cursor().capacity == 4096);

    CHECK(co_await inline_round_trip(handle, 2048, 1)); // fits the small ring

    handle->upload.set_inline_budget(isize(128) * 1024); // grow the ring
    handle->advance_epoch();                             // applies the pending budget
    CHECK(handle->_upload_inline.debug_cursor().capacity == isize(128) * 1024);

    CHECK(co_await inline_round_trip(handle, isize(64) * 1024, 2)); // would not fit the 4 KiB ring
}

ASYNC_INVOCABLE_TEST("sg dx12 - inline download ring grows to fit a larger readback",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    // A 4 KiB readback ring: a download larger than this asserts before the resize.
    auto const sizes = dx12::scoped_transfer_sizes(*handle, {.download_ring_bytes = 4096});
    REQUIRE(handle->_download_inline.debug_cursor().capacity == 4096);

    CHECK(co_await inline_round_trip(handle, 2048, 1)); // fits the small ring

    handle->download.set_budget(isize(128) * 1024); // grow the readback ring
    handle->advance_epoch();                        // applies the pending budget (drains the actor)
    CHECK(handle->_download_inline.debug_cursor().capacity == isize(128) * 1024);

    CHECK(co_await inline_round_trip(handle, isize(64) * 1024, 2)); // would not fit the 4 KiB ring
}
