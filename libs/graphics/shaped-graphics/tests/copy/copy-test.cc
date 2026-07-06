#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic device→device buffer copy (cmd.copy) over the public sg API, run against every available
// backend (see tests/context/context-test.cc for the invocable/alias mechanism).

namespace
{
auto pattern = [](int i) { return cc::byte(i & 0xFF); };

sg::buffer_handle make_copy_buffer(sg::context_handle const& ctx, cc::isize size)
{
    auto buf = ctx->persistent.create_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    CC_ASSERT(buf.has_value(), "copy test buffer allocation failed");
    return buf.value();
}
} // namespace

INVOCABLE_TEST("sg - copies a buffer in one list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, 256);
    auto const dst = make_copy_buffer(ctx, 256);

    cc::byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);

    // Single list: upload into src, copy src→dst, read dst back — the backend orders all three.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(src, cc::span<cc::byte const>(data, 256));
    cmd.value()->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    auto future = cmd.value()->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - copies a buffer across separate lists", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, 256);
    auto const dst = make_copy_buffer(ctx, 256);

    cc::byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);

    auto up = ctx->create_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(src, cc::span<cc::byte const>(data, 256));
    ctx->submit_command_list(cc::move(up.value()));

    auto cp = ctx->create_command_list();
    REQUIRE(cp.has_value());
    cp.value()->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    ctx->submit_command_list(cc::move(cp.value()));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[200] == pattern(200));
}

INVOCABLE_TEST("sg - copies a sub-range with offsets", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, 256);
    auto const dst = make_copy_buffer(ctx, 256);

    cc::byte data[256];
    for (int i = 0; i < 256; ++i)
        data[i] = pattern(i);

    // Copy src[64,128) into dst[128,192); read back exactly that window.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(src, cc::span<cc::byte const>(data, 256));
    cmd.value()->copy.buffer_bytes_region(
        {.src = src, .dst = dst, .size_in_bytes = 64, .src_offset_in_bytes = 64, .dst_offset_in_bytes = 128});
    auto future = cmd.value()->download.bytes_from_buffer(dst, 128, 64);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 64);
    bool matches = true;
    for (int i = 0; i < 64; ++i)
        if (bytes.value()[i] != pattern(64 + i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - typed copy in element units", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, cc::isize(8) * sizeof(int));
    auto const dst = make_copy_buffer(ctx, cc::isize(8) * sizeof(int));

    int const in[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    // Copy in[2,6) into dst[0,4): count / offsets are in elements of int.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.data_to_buffer(src, cc::span<int const>(in, 8));
    cmd.value()->copy.buffer_data_region<int>({.src = src, .dst = dst, .count = 4, .src_offset = 2, .dst_offset = 0});
    auto future = cmd.value()->download.data_from_buffer<int>(dst, 0, 4);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const data = future.wait_get_data();
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 3);
    CHECK(data.value()[3] == 6);
}

INVOCABLE_TEST("sg - zero-size copy leaves the destination untouched", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_copy_buffer(ctx, 16);
    auto const dst = make_copy_buffer(ctx, 16);

    cc::byte src_data[16];
    cc::byte dst_data[16];
    for (int i = 0; i < 16; ++i)
    {
        src_data[i] = cc::byte(0xAA);
        dst_data[i] = cc::byte(0xBB);
    }

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(src, cc::span<cc::byte const>(src_data, 16));
    cmd.value()->upload.bytes_to_buffer(dst, cc::span<cc::byte const>(dst_data, 16));
    cmd.value()->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 0}); // no-op
    auto future = cmd.value()->download.bytes_from_buffer(dst, 0, 16);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    bool untouched = true;
    for (int i = 0; i < 16; ++i)
        if (bytes.value()[i] != cc::byte(0xBB))
            untouched = false;
    CHECK(untouched);
}
