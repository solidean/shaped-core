#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

// dx12 async-upload internals that the backend-agnostic tier-1 suite (tests/transfer/upload-async-test.cc)
// can't reach: the staging pipeline's window packing and recycling, forced with a deliberately tiny window
// size (a dx12_config knob). The public upload/sync contract is pinned in tier 1. See
// libs/graphics/shaped-graphics/docs/testing.md and libs/graphics/shaped-graphics/docs/concepts/upload.async.md.

namespace
{
namespace dx12 = sg::backend::dx12;

// A pinned byte buffer filled by fn(i), moved into the pin (owns it, zero-copy).
cc::pinned_data<cc::byte const> make_bytes(cc::isize n, auto&& fn)
{
    cc::vector<cc::byte> data;
    data.reserve(n);
    for (cc::isize i = 0; i < n; ++i)
        data.push_back(cc::byte(fn(i)));
    return cc::make_pinned_data(cc::move(data));
}
} // namespace

// A single upload larger than one staging window must pack across several windows, pipelining and
// recycling as it goes. A fresh context with deliberately tiny windows forces it.
TEST("sg dx12 - async upload larger than a staging window packs across windows")
{
    auto ctx = sg::create_dx12_context({.use_warp = true, .async_upload_window_bytes = 4096});
    REQUIRE(ctx.has_value());
    auto& c = static_cast<dx12::dx12_context&>(*ctx.value());

    cc::isize const n = 20000; // several windows, non-aligned so partial windows are exercised
    auto buf = c.create_dx12_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());

    c.upload.bytes_to_buffer(buf.value(), make_bytes(n, [](cc::isize i) { return i * 7 + 1; }));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf.value(), 0, n);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == n);
    bool matches = true;
    for (cc::isize i = 0; i < n; ++i)
        if (bytes.value()[i] != cc::byte(i * 7 + 1))
            matches = false;
    CHECK(matches);
}

// Many uploads whose aggregate far exceeds the staging buffer must all land, forcing the actor to wait on
// the window fence and recycle windows repeatedly. Each targets its own buffer; all must read back intact.
// Regression: window 0 shares its staging slot with window 3, and the window fence must observe window 0's
// completion before window 3 overwrites the slot — a 0-based fence value collided with the fence's initial
// 0 and skipped that wait, so buffer 0 read back buffer 3's bytes (only slot 0 was affected).
TEST("sg dx12 - many async uploads recycle the staging windows")
{
    auto ctx = sg::create_dx12_context({.use_warp = true, .async_upload_window_bytes = 1024});
    REQUIRE(ctx.has_value());
    auto& c = static_cast<dx12::dx12_context&>(*ctx.value());

    int const count = 24;
    cc::isize const each = 1024;
    cc::vector<sg::raw_buffer_handle> bufs;
    for (int k = 0; k < count; ++k)
    {
        auto buf
            = c.create_dx12_buffer(each, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
        REQUIRE(buf.has_value());
        c.upload.bytes_to_buffer(buf.value(), make_bytes(each, [k](cc::isize i) { return i + k; }));
        bufs.push_back(buf.value());
    }

    bool all_ok = true;
    for (int k = 0; k < count; ++k)
    {
        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download.bytes_from_buffer(bufs[k], 0, each);
        c.submit_dx12_command_list(cc::move(down.value()));

        auto const bytes = c.wait_for(future);
        REQUIRE(bytes.has_value());
        for (cc::isize i = 0; i < each; ++i)
            if (bytes.value()[i] != cc::byte(i + k))
                all_ok = false;
    }
    CHECK(all_ok);
}

// Uneven upload sizes (none a window multiple) force the actor to both pack several jobs into one window and
// split a single job across windows, all while recycling — a shape the exact-fill and single-large-upload
// tests miss. Distinct buffers; each must read back intact.
TEST("sg dx12 - uneven async uploads pack and straddle staging windows")
{
    auto ctx = sg::create_dx12_context({.use_warp = true, .async_upload_window_bytes = 1024});
    REQUIRE(ctx.has_value());
    auto& c = static_cast<dx12::dx12_context&>(*ctx.value());

    // Sizes chosen so windows carry parts of two uploads and single uploads span windows; aggregate spans
    // several windows so slots recycle. Byte k*13+7 keeps each buffer's pattern distinct.
    cc::isize const sizes[] = {700, 300, 900, 1500, 200, 1100, 640, 1300, 480, 760};
    int const count = 10;
    cc::vector<sg::raw_buffer_handle> bufs;
    for (int k = 0; k < count; ++k)
    {
        cc::isize const n = sizes[k];
        auto buf
            = c.create_dx12_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
        REQUIRE(buf.has_value());
        c.upload.bytes_to_buffer(buf.value(), make_bytes(n, [k](cc::isize i) { return i * (k * 13 + 7); }));
        bufs.push_back(buf.value());
    }

    bool all_ok = true;
    for (int k = 0; k < count; ++k)
    {
        cc::isize const n = sizes[k];
        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download.bytes_from_buffer(bufs[k], 0, n);
        c.submit_dx12_command_list(cc::move(down.value()));

        auto const bytes = c.wait_for(future);
        REQUIRE(bytes.has_value());
        REQUIRE(bytes.value().size() == n);
        for (cc::isize i = 0; i < n; ++i)
            if (bytes.value()[i] != cc::byte(i * (k * 13 + 7)))
                all_ok = false;
    }
    CHECK(all_ok);
}
