#include "webgpu-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread_pump.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// ctx.stream's pacing on WebGPU, where every transfer shares the main thread with the frame.
// A pump sweep moves at most one window of bytes, in either direction, however large the stream.
// Each test shrinks the window so a small payload spans many windows, and restores it before it ends.

namespace
{
namespace webgpu = sg::backend::webgpu;

constexpr isize k_window = 1024;
constexpr isize k_default_window = 4 * 1024 * 1024;

[[nodiscard]] cc::vector<byte> pattern(isize n)
{
    auto out = cc::vector<byte>();
    for (isize i = 0; i < n; ++i)
        out.push_back(byte(i * 7 + 3));
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg webgpu - a stream upload moves at most one window per pump sweep",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    ctx._streams.set_window_bytes(k_window);

    constexpr isize size = 16 * k_window;
    auto const src = pattern(size);
    auto buf = ctx.persistent.create_raw_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto stream = ctx.stream.bytes_to_buffer(buf, cc::make_pinned_data(src));

    // One sweep: one chunk holds the whole payload, and the window must cut it.
    (void)cc::thread_pump_all();
    auto const after_one = stream.progress().bytes_done;
    CHECK(after_one > 0);
    CHECK(after_one <= k_window);

    REQUIRE((co_await cc::async_as_result(stream.completion())).has_value());
    auto const back = ctx.download.bytes_from_buffer(buf, 0, size);
    auto const bytes = co_await back.bytes();
    REQUIRE(bytes.size() == size);
    CHECK(cc::memcmp(bytes.data(), src.data(), size_t(size)) == 0);
    ctx._streams.set_window_bytes(k_default_window);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a stream download reaches its sink a window at a time, in order",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    ctx._streams.set_window_bytes(k_window);

    constexpr isize size = 8 * k_window + 12; // a short last window
    auto const src = pattern(size);
    auto buf = ctx.persistent.create_raw_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    {
        auto cmd = ctx.create_command_list();
        cmd->upload.bytes_to_buffer(buf, src);
        ctx.submit_command_list(cc::move(cmd));
    }

    struct received
    {
        isize offset = 0;
        isize size = 0;
    };
    auto chunks = cc::vector<received>();
    auto collected = cc::vector<byte>();
    auto stream = ctx.stream.to_sink_from_buffer(
        buf,
        [&](cc::span<byte const> bytes, isize offset)
        {
            chunks.push_back({.offset = offset, .size = bytes.size()});
            for (auto const b : bytes)
                collected.push_back(b);
            return true;
        },
        0, size);

    REQUIRE((co_await cc::async_as_result(stream.completion())).has_value());
    CHECK(chunks.size() >= 8);
    auto expected = isize(0);
    for (auto const& c : chunks)
    {
        CHECK(c.offset == expected);
        CHECK(c.size <= k_window);
        expected += c.size;
    }
    CHECK(expected == size);
    REQUIRE(collected.size() == size);
    CHECK(cc::memcmp(collected.data(), src.data(), size_t(size)) == 0);
    ctx._streams.set_window_bytes(k_default_window);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a texture streams both ways in whole rows across many windows",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    ctx._streams.set_window_bytes(k_window);

    // 256-byte rows, so a 1 KiB window holds four of the 32.
    constexpr int width = 64;
    constexpr int height = 32;
    auto texture = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                     .width = width,
                                                     .height = height,
                                                     .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst});
    auto const texels = pattern(isize(width) * height * 4);

    auto up = ctx.stream.bytes_to_texture(texture.raw(), cc::make_pinned_data(texels));
    (void)cc::thread_pump_all();
    CHECK(up.progress().bytes_done <= k_window);
    REQUIRE((co_await cc::async_as_result(up.completion())).has_value());

    auto down = ctx.stream.bytes_from_texture(texture.raw());
    REQUIRE((co_await cc::async_as_result(down.completion())).has_value());
    auto const bytes = co_await down.future().bytes();
    REQUIRE(bytes.size() == texels.size());
    CHECK(cc::memcmp(bytes.data(), texels.data(), size_t(texels.size())) == 0);
    ctx._streams.set_window_bytes(k_default_window);
}
