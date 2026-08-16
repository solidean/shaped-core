#include "dx12-test-common.hh"

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// dx12 async-download internals the backend-agnostic tier-1 suite (tests/transfer/download-async-test.cc) can't reach.
// That is the readback staging pipeline's window packing and recycling, forced with a deliberately tiny window size (a dx12_config knob).
// The public download/sync contract is pinned in tier 1.
// See libs/graphics/shaped-graphics/docs/testing.md and libs/graphics/shaped-graphics/docs/concepts/download.async.md.

namespace
{
namespace dx12 = sg::backend::dx12;

// Seeds `buf` with fn(i) via an inline command-list upload on the direct queue and submits it, so the async download reads committed bytes by auto-waiting on the seed list.
void seed(sg::context& c, sg::raw_buffer_handle const& buf, isize n, auto&& fn)
{
    cc::vector<byte> data;
    data.reserve(n);
    for (isize i = 0; i < n; ++i)
        data.push_back(byte(fn(i)));

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<byte const>(data));
    c.submit_command_list(cc::move(up));
}
} // namespace

// A single download larger than one readback window must pack across several windows, pipelining and recycling as it goes.
// A fresh context with deliberately tiny windows forces it.
TEST("sg dx12 - async download larger than a staging window packs across windows", exclusive("gpu"))
{
    auto ctx = dx12::make_test_context({.async_download_window_bytes = 4096});
    REQUIRE(ctx.has_value());
    auto& c = *ctx.value();

    isize const n = 20000; // several windows, non-aligned so partial windows are exercised
    auto buf = c.persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);
    seed(c, buf, n, [](isize i) { return i * 7 + 1; });

    auto future = c.download.bytes_from_buffer(buf, 0, n);
    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == n);
    bool matches = true;
    for (isize i = 0; i < n; ++i)
        if (bytes.value()[i] != byte(i * 7 + 1))
            matches = false;
    CHECK(matches);
}

// Many downloads whose aggregate far exceeds the staging buffer must all land, forcing the actor to wait on the window fence and recycle windows repeatedly.
// Each targets its own buffer; all must read back intact.
TEST("sg dx12 - many async downloads recycle the staging windows", exclusive("gpu"))
{
    auto ctx = dx12::make_test_context({.async_download_window_bytes = 1024});
    REQUIRE(ctx.has_value());
    auto& c = *ctx.value();

    int const count = 24;
    isize const each = 1024;
    cc::vector<sg::raw_buffer_handle> bufs;
    for (int k = 0; k < count; ++k)
    {
        auto buf = c.persistent.create_raw_buffer(each, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
        REQUIRE(buf != nullptr);
        seed(c, buf, each, [k](isize i) { return i + k; });
        bufs.push_back(buf);
    }

    bool all_ok = true;
    for (int k = 0; k < count; ++k)
    {
        auto future = c.download.bytes_from_buffer(bufs[k], 0, each);
        auto const bytes = c.wait_for(future);
        REQUIRE(bytes.has_value());
        for (isize i = 0; i < each; ++i)
            if (bytes.value()[i] != byte(i + k))
                all_ok = false;
    }
    CHECK(all_ok);
}

// Uneven download sizes (none a window multiple) force the actor to both pack several reads into one window and split a single read across windows, all while recycling.
// That is a shape the exact-fill and single-large tests miss.
// Distinct buffers; each must read back intact.
TEST("sg dx12 - uneven async downloads pack and straddle staging windows", exclusive("gpu"))
{
    auto ctx = dx12::make_test_context({.async_download_window_bytes = 1024});
    REQUIRE(ctx.has_value());
    auto& c = *ctx.value();

    isize const sizes[] = {700, 300, 900, 1500, 200, 1100, 640, 1300, 480, 760};
    int const count = 10;
    cc::vector<sg::raw_buffer_handle> bufs;
    for (int k = 0; k < count; ++k)
    {
        isize const n = sizes[k];
        auto buf = c.persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
        REQUIRE(buf != nullptr);
        seed(c, buf, n, [k](isize i) { return i * (k * 13 + 7); });
        bufs.push_back(buf);
    }

    bool all_ok = true;
    for (int k = 0; k < count; ++k)
    {
        isize const n = sizes[k];
        auto future = c.download.bytes_from_buffer(bufs[k], 0, n);
        auto const bytes = c.wait_for(future);
        REQUIRE(bytes.has_value());
        REQUIRE(bytes.value().size() == n);
        for (isize i = 0; i < n; ++i)
            if (bytes.value()[i] != byte(i * (k * 13 + 7)))
                all_ok = false;
    }
    CHECK(all_ok);
}
