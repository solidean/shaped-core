#include "dx12-test-common.hh"

#include <nexus/test.hh>

// Device→device buffer copy: cmd.copy.buffer_bytes_region / buffer_data_region, on WARP. These tests
// split upload → copy → download across separate command lists; recording all three in one list is also
// correct now (dx12_command_list::record_transfer_barrier orders them) — the backend-agnostic tests
// (tests/copy/) cover that single-list path. See libs/graphics/shaped-graphics/docs/concepts/backends.md.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - buffer copy round-trips")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto src = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    cc::byte pattern[256];
    for (int i = 0; i < 256; ++i)
        pattern[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(src, cc::span<cc::byte const>(pattern, 256));
    c.submit_command_list(cc::move(up));

    auto cp = c.create_command_list();
    REQUIRE(cp != nullptr);
    cp->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    c.submit_command_list(cc::move(cp));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(dst, 0, 256);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != cc::byte(i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - buffer copy with offsets")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto src = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    cc::byte pattern[256];
    for (int i = 0; i < 256; ++i)
        pattern[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(src, cc::span<cc::byte const>(pattern, 256));
    c.submit_command_list(cc::move(up));

    // Copy src[64,128) into dst[128,192).
    auto cp = c.create_command_list();
    REQUIRE(cp != nullptr);
    cp->copy.buffer_bytes_region(
        {.src = src, .dst = dst, .size_in_bytes = 64, .src_offset_in_bytes = 64, .dst_offset_in_bytes = 128});
    c.submit_command_list(cc::move(cp));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(dst, 128, 64);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 64);
    bool matches = true;
    for (int i = 0; i < 64; ++i)
        if (bytes.value()[i] != cc::byte(64 + i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - typed buffer_data_region copy")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const bytes = cc::isize(4) * sizeof(int);
    auto src = c.persistent.create_raw_buffer(bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = c.persistent.create_raw_buffer(bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    int const in[4] = {5, 6, 7, 8};
    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.data_to_buffer(src, cc::span<int const>(in, 4));
    c.submit_command_list(cc::move(up));

    auto cp = c.create_command_list();
    REQUIRE(cp != nullptr);
    cp->copy.buffer_data_region<int>({.src = src, .dst = dst, .count = 4});
    c.submit_command_list(cc::move(cp));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<int>(dst, 0, 4);
    c.submit_command_list(cc::move(down));

    auto const data = c.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 5);
    CHECK(data.value()[3] == 8);
}

TEST("sg dx12 - typed buffer_data_region copy with element offsets")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const bytes = cc::isize(8) * sizeof(int);
    auto src = c.persistent.create_raw_buffer(bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = c.persistent.create_raw_buffer(bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    int const in[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.data_to_buffer(src, cc::span<int const>(in, 8));
    c.submit_command_list(cc::move(up));

    // Copy 3 elements from src[2..5) to dst[4..7) — offsets are in elements of int.
    auto cp = c.create_command_list();
    REQUIRE(cp != nullptr);
    cp->copy.buffer_data_region<int>({.src = src, .dst = dst, .count = 3, .src_offset = 2, .dst_offset = 4});
    c.submit_command_list(cc::move(cp));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<int>(dst, 4, 3);
    c.submit_command_list(cc::move(down));

    auto const data = c.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 3);
    CHECK(data.value()[0] == 12); // src[2]
    CHECK(data.value()[1] == 13); // src[3]
    CHECK(data.value()[2] == 14); // src[4]
}

TEST("sg dx12 - same-buffer non-overlapping copy")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::byte pattern[256];
    for (int i = 0; i < 256; ++i)
        pattern[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(pattern, 256));
    c.submit_command_list(cc::move(up));

    // Copy [0,64) → [128,192) within the same buffer (ranges do not overlap).
    auto cp = c.create_command_list();
    REQUIRE(cp != nullptr);
    cp->copy.buffer_bytes_region(
        {.src = buf, .dst = buf, .size_in_bytes = 64, .src_offset_in_bytes = 0, .dst_offset_in_bytes = 128});
    c.submit_command_list(cc::move(cp));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 128, 64);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 64);
    bool matches = true;
    for (int i = 0; i < 64; ++i)
        if (bytes.value()[i] != cc::byte(i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - zero-size copy is a no-op")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto src = c.persistent.create_raw_buffer(16, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = c.persistent.create_raw_buffer(16, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 0}); // no-op
    c.submit_command_list(cc::move(cmd));
}
