#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

// Inline transfer, smallest case first.
// Split one-list from two-list deliberately: the second needs the cross-list queue barrier pair and the first does not.
// A failure then tells you which half is wrong rather than only that a round trip broke.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines; // the sized aliases are vocabulary; a test file pulls them in once

namespace
{
constexpr auto k_copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

[[nodiscard]] cc::vector<byte> pattern_bytes(int seed, int count)
{
    auto out = cc::vector<byte>::create_uninitialized(count);
    for (auto i = 0; i < count; ++i)
        out[i] = byte(u8((seed + i * 7) & 0xFF));
    return out;
}
} // namespace

ASYNC_TEST("sg metal - an upload and a download in one list round-trip")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const buffer = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(1, 256);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(buffer, source);
    auto future = cmd->download.bytes_from_buffer(buffer, 0, 256);
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

ASYNC_TEST("sg metal - an upload and a download in two lists round-trip")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The cross-list case: the write is in one command buffer and the read in the next, so the ordering rests entirely
    // on the queue barrier pair the encoder carries.
    // Metal has no implicit decay to fall back on.
    auto const buffer = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(2, 256);

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buffer, source);
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    auto future = down->download.bytes_from_buffer(buffer, 0, 256);
    ctx->submit_command_list(cc::move(down));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

ASYNC_TEST("sg metal - a device-to-device copy moves the bytes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const src = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const dst = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(3, 256);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(src, source);
    cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    auto future = cmd->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

ASYNC_TEST("sg metal - a copy between two lists sees the previous list's write")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Three lists, which is the shape the tier-1 copy suite uses: the middle one both reads what the first wrote and
    // writes what the third reads, so it needs the queue barrier pair on both sides.
    auto const src = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const dst = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(5, 256);

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(src, source);
    ctx->submit_command_list(cc::move(up));

    auto cp = ctx->create_command_list();
    cp->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    ctx->submit_command_list(cc::move(cp));

    auto down = ctx->create_command_list();
    auto future = down->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(down));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

ASYNC_TEST("sg metal - a copy within one buffer moves the bytes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // src and dst are the same buffer, at non-overlapping ranges — which sg allows and which declares both a read and
    // a write on one resource for a single op.
    auto const buffer = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(6, 256);

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buffer, source);
    ctx->submit_command_list(cc::move(up));

    auto cmd = ctx->create_command_list();
    cmd->copy.buffer_bytes_region(
        {.src = buffer, .dst = buffer, .size_in_bytes = 64, .src_offset_in_bytes = 0, .dst_offset_in_bytes = 128});
    auto future = cmd->download.bytes_from_buffer(buffer, 128, 64);
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 64);

    auto mismatches = 0;
    for (auto i = 0; i < 64; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0)
        .context(cc::format("{} of 64 bytes differ; first read back as {}, source[0]={}, source[128]={}", mismatches,
                            int(u8(bytes.value()[0])), int(u8(source[0])), int(u8(source[128]))));
}
