#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/types.hh>

using namespace cc::primitive_defines;

// Backend-agnostic device→device buffer copy (cmd.copy) over the public sg API.
// Run against every available backend — see tests/context/context-test.cc for the invocable/alias mechanism.

namespace
{
auto pattern = [](int i) { return byte(i & 0xFF); };

sg::raw_buffer_handle make_copy_buffer(sg::context_handle const& ctx, isize size)
{
    auto buf = ctx->persistent.create_raw_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    CC_ASSERT(buf != nullptr, "copy test buffer allocation failed");
    return buf;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - copies a buffer in one list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, 256);
    auto const dst = make_copy_buffer(ctx, 256);

    byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);

    // Single list: upload into src, copy src→dst, read dst back — the backend orders all three.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_buffer(src, cc::span<byte const>(data, 256));
    cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    auto future = cmd->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

ASYNC_INVOCABLE_TEST("sg - copies a buffer across separate lists", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, 256);
    auto const dst = make_copy_buffer(ctx, 256);

    byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);

    auto up = ctx->create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(src, cc::span<byte const>(data, 256));
    ctx->submit_command_list(cc::move(up));

    auto cp = ctx->create_command_list();
    REQUIRE(cp != nullptr);
    cp->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    ctx->submit_command_list(cc::move(cp));

    auto down = ctx->create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(down));

    auto const bytes = co_await future.bytes();
    CHECK(bytes[200] == pattern(200));
}

ASYNC_INVOCABLE_TEST("sg - copies a sub-range with offsets", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);
    auto const src = ctx->persistent.create_buffer_from_bytes(data, sg::buffer_usage::copy_src);
    auto const dst = make_copy_buffer(ctx, 256);

    // Copy src[64,128) into dst[128,192); read back exactly that window.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->copy.buffer_bytes_region(
        {.src = src, .dst = dst, .size_in_bytes = 64, .src_offset_in_bytes = 64, .dst_offset_in_bytes = 128});
    auto future = cmd->download.bytes_from_buffer(dst, 128, 64);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == 64);
    bool matches = true;
    for (int i = 0; i < 64; ++i)
        if (bytes[i] != pattern(64 + i))
            matches = false;
    CHECK(matches);
}

// A same-buffer copy is the one op that reads and writes one resource at once.
// It must be the FIRST use of the buffer in its list, which is what the fuzz test found: with nothing in flight the tracker used to skip the barrier and let the backend infer the access,
// and D3D12 can only infer one — it assumed COPY_DEST and rejected the source read.
ASYNC_INVOCABLE_TEST("sg - copies within one buffer on its first use in a list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);

    // Filled through ctx.upload rather than a list, so the copy list below starts with no tracked access for this buffer.
    auto const buf = ctx->persistent.create_buffer_from_bytes(data, sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->copy.buffer_bytes_region(
        {.src = buf, .dst = buf, .size_in_bytes = 64, .src_offset_in_bytes = 0, .dst_offset_in_bytes = 128});
    auto future = cmd->download.bytes_from_buffer(buf, 128, 64);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == 64);
    bool matches = true;
    for (int i = 0; i < 64; ++i)
        if (bytes[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

ASYNC_INVOCABLE_TEST("sg - typed copy in element units", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    int const in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto const src = ctx->persistent.create_buffer_from_data(in, sg::buffer_usage::copy_src);
    auto const dst = make_copy_buffer(ctx, isize(8) * sizeof(int));

    // Copy in[2,6) into dst[0,4): count / offsets are in elements of int.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->copy.buffer_data_region<int>({.src = src.raw(), .dst = dst, .count = 4, .src_offset = 2, .dst_offset = 0});
    auto future = cmd->download.data_from_buffer<int>(dst, 0, 4);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 4);
    CHECK(data[0] == 3);
    CHECK(data[3] == 6);
}

ASYNC_INVOCABLE_TEST("sg - zero-size copy leaves the destination untouched", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    byte src_data[16];
    byte dst_data[16];
    for (int i = 0; i < 16; ++i)
    {
        src_data[i] = byte(0xAA);
        dst_data[i] = byte(0xBB);
    }

    auto const src = ctx->persistent.create_buffer_from_bytes(src_data, sg::buffer_usage::copy_src);
    auto const dst = ctx->persistent.create_buffer_from_bytes(dst_data, sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 0}); // no-op
    auto future = cmd->download.bytes_from_buffer(dst, 0, 16);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    bool untouched = true;
    for (int i = 0; i < 16; ++i)
        if (bytes[i] != byte(0xBB))
            untouched = false;
    CHECK(untouched);
}
