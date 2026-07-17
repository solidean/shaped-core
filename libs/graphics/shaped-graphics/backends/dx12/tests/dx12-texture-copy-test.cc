#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>

// Inline texture copy on WARP: upload tightly-packed pixels into a texture, read them back, and verify
// the round-trip — exercising the 256/512 row/placement padding, the region path, block-compressed
// formats, and the copy_dst/copy_src layout barriers the copy drives.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::texture_description copy_desc(sg::pixel_format fmt, int w, int h)
{
    sg::texture_description d;
    d.format = fmt;
    d.dimension = sg::texture_dimension::d2;
    d.width = w;
    d.height = h;
    d.usage = sg::texture_usage::copy_dst | sg::texture_usage::copy_src;
    return d;
}
} // namespace

TEST("sg dx12 - texture footprint math (padding, subresource index, block sizing)")
{
    // compute_texture_footprint takes an already-resolved concrete region (the sg layer expands "whole
    // subresource" and skips empty regions), so these pass explicit boxes.

    // 8×8 RGBA8, mip 0: tight row 32 bytes -> padded to 256; whole subresource.
    {
        sg::texture_description const d = copy_desc(sg::pixel_format::rgba8_unorm, 8, 8);
        auto const fp = dx12::compute_texture_footprint(
            d, {}, sg::texture_region{.offset = tg::pos3i(0, 0, 0), .size = tg::vec3i(8, 8, 1)});
        CHECK(fp.subresource == 0u);
        CHECK(fp.width == 8);
        CHECK(fp.height == 8);
        CHECK(fp.depth == 1);
        CHECK(fp.rows == 8);
        CHECK(fp.depth_slices == 1);
        CHECK(fp.row_bytes == 32);
        CHECK(fp.padded_pitch == 256); // aligned up from 32
        CHECK(fp.tight_size() == 32 * 8);
        CHECK(fp.staged_size() == 256 * 8);
    }

    // A 4×3 region of the same texture — the row is still padded, but only 3 rows are staged.
    {
        sg::texture_description const d = copy_desc(sg::pixel_format::rgba8_unorm, 8, 8);
        auto const fp = dx12::compute_texture_footprint(
            d, {}, sg::texture_region{.offset = tg::pos3i(2, 1, 0), .size = tg::vec3i(4, 3, 1)});
        CHECK(fp.x == 2);
        CHECK(fp.y == 1);
        CHECK(fp.width == 4);
        CHECK(fp.height == 3);
        CHECK(fp.row_bytes == 16);
        CHECK(fp.rows == 3);
        CHECK(fp.tight_size() == 16 * 3);
        CHECK(fp.staged_size() == 256 * 3);
    }

    // mip 1 of an array texture: extents halve, and the subresource index accounts for the layer.
    {
        sg::texture_description d = copy_desc(sg::pixel_format::r32_float, 8, 8);
        d.mip_levels = 4;
        d.array_layers = 3;
        auto const fp = dx12::compute_texture_footprint(
            d, {.mip_level = 1, .array_layer = 2},
            sg::texture_region{.offset = tg::pos3i(0, 0, 0), .size = tg::vec3i(4, 4, 1)});
        CHECK(fp.subresource == 1u + 2u * 4u); // mip + layer*mipLevels
        CHECK(fp.width == 4);
        CHECK(fp.height == 4);
        CHECK(fp.row_bytes == 16); // 4 texels * 4 bytes
    }

    // Block-compressed: rows and widths are counted in 4×4 blocks. BC1 = 8 bytes/block.
    {
        sg::texture_description const d = copy_desc(sg::pixel_format::bc1_rgba_unorm, 16, 16);
        auto const fp = dx12::compute_texture_footprint(
            d, {}, sg::texture_region{.offset = tg::pos3i(0, 0, 0), .size = tg::vec3i(16, 16, 1)});
        CHECK(fp.rows == 4);          // 16 / 4 block-rows
        CHECK(fp.row_bytes == 4 * 8); // 4 blocks wide * 8 bytes
        CHECK(fp.padded_pitch == 256);
    }
}

TEST("sg dx12 - texture upload/download round-trip pads + un-pads rows")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    constexpr int W = 8, H = 8, N = W * H;
    auto tex = c.persistent.create_raw_texture(copy_desc(sg::pixel_format::r32_float, W, H));
    REQUIRE(tex != nullptr);

    // 8×8 R32_FLOAT: a tight row is 32 bytes, staged at a 256-byte pitch — so this exercises the padding.
    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) + 0.5f;

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_texture(tex, cc::as_bytes(cc::span<float const>(src, N)));
    auto future = cmd->download.bytes_from_texture(tex);
    c.submit_command_list(cc::move(cmd));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(sizeof(src)));
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - texture upload into a sub-region leaves the rest untouched")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    constexpr int W = 8, H = 8, N = W * H;
    auto tex = c.persistent.create_raw_texture(copy_desc(sg::pixel_format::r32_float, W, H));
    REQUIRE(tex != nullptr);

    float zeros[N] = {};
    // A 4×3 patch of 1..12, uploaded at offset (2, 1).
    constexpr int RW = 4, RH = 3;
    float patch[RW * RH];
    for (int i = 0; i < RW * RH; ++i)
        patch[i] = float(i + 1);

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_texture(tex, cc::as_bytes(cc::span<float const>(zeros, N)));
    cmd->upload.bytes_to_texture(tex, cc::as_bytes(cc::span<float const>(patch, RW * RH)), {},
                                 sg::texture_region{.offset = tg::pos3i(2, 1, 0), .size = tg::vec3i(RW, RH, 1)});
    auto future = cmd->download.bytes_from_texture(tex);
    c.submit_command_list(cc::move(cmd));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            bool const in_patch = x >= 2 && x < 2 + RW && y >= 1 && y < 1 + RH;
            float const expected = in_patch ? patch[(y - 1) * RW + (x - 2)] : 0.0f;
            if (got[y * W + x] != expected)
                ok = false;
        }
    CHECK(ok);
}

TEST("sg dx12 - async texture upload/download round-trip on the copy queue")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    constexpr int W = 8, H = 8, N = W * H;
    auto tex = c.persistent.create_raw_texture(copy_desc(sg::pixel_format::r32_float, W, H));
    REQUIRE(tex != nullptr);

    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) + 0.25f;

    // ctx.upload / ctx.download run on dedicated copy queues; the readback waits on the upload's completion
    // fence automatically (both cross-queue syncs go through the texture's pending stamps).
    handle->upload.bytes_to_texture(tex, cc::make_pinned_data(cc::as_bytes(cc::span<float const>(src, N))));
    auto future = handle->download.bytes_from_texture(tex);

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(sizeof(src)));
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - an inline readback waits on a pending async texture upload")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    constexpr int W = 8, H = 8, N = W * H;
    auto tex = c.persistent.create_raw_texture(copy_desc(sg::pixel_format::r32_float, W, H));
    REQUIRE(tex != nullptr);

    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) * 2.0f + 1.0f;

    // Async upload on the copy queue, then an inline readback in a direct-queue list. The inline list must
    // wait on the async upload's completion fence (folded in via track_texture_access) to observe the write.
    handle->upload.bytes_to_texture(tex, cc::make_pinned_data(cc::as_bytes(cc::span<float const>(src, N))));

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    auto future = cmd->download.bytes_from_texture(tex);
    c.submit_command_list(cc::move(cmd));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - texture copy chunking (2D split, 3D whole-slice batch + mid-slice split)")
{
    // 2D — one depth slice, 8 block-rows. A window that fits fewer rows than the whole slice yields a
    // partial row-run; the next call finishes the slice.
    {
        dx12::dx12_texture_footprint fp;
        fp.rows = 8;
        fp.depth_slices = 1;
        fp.padded_pitch = 256;

        auto const a = dx12::next_texture_copy_chunk(fp, 0, 0, 3); // room for 3 rows
        CHECK(a.slice_start == 0);
        CHECK(a.slice_count == 1);
        CHECK(a.row_start == 0);
        CHECK(a.row_count == 3);
        CHECK(a.staging_rows() == 3);

        auto const b = dx12::next_texture_copy_chunk(fp, 0, 3, 100); // rest of the slice
        CHECK(b.slice_count == 1);
        CHECK(b.row_start == 3);
        CHECK(b.row_count == 5);
    }

    // 3D — 4 rows/slice, 3 slices. A big window batches whole slices into one chunk; a small one splits.
    {
        dx12::dx12_texture_footprint fp;
        fp.rows = 4;
        fp.depth_slices = 3;
        fp.padded_pitch = 256;

        auto const all = dx12::next_texture_copy_chunk(fp, 0, 0, 100); // everything fits -> one chunk
        CHECK(all.slice_start == 0);
        CHECK(all.slice_count == 3);
        CHECK(all.row_start == 0);
        CHECK(all.row_count == 4);
        CHECK(all.staging_rows() == 12);

        auto const one = dx12::next_texture_copy_chunk(fp, 0, 0, 5); // room for exactly one whole slice
        CHECK(one.slice_count == 1);
        CHECK(one.row_start == 0);
        CHECK(one.row_count == 4);

        auto const part = dx12::next_texture_copy_chunk(fp, 0, 0, 3); // less than a slice -> partial rows
        CHECK(part.slice_count == 1);
        CHECK(part.row_start == 0);
        CHECK(part.row_count == 3);

        auto const mid = dx12::next_texture_copy_chunk(fp, 1, 2, 5); // mid-slice cursor -> finish that slice
        CHECK(mid.slice_start == 1);
        CHECK(mid.slice_count == 1);
        CHECK(mid.row_start == 2);
        CHECK(mid.row_count == 2);
    }
}

TEST("sg dx12 - inline texture upload splits across the ring seam")
{
    // A ring that holds the region + its staging slack (tight 768 + padded 256 + 512 alignment = 1536).
    // Parking the cursor 512 bytes before the seam forces the 768-byte region to split — 2 padded rows
    // before the seam, 1 after.
    constexpr cc::isize ring_bytes = 2048;
    constexpr cc::isize park = ring_bytes - 512; // 512 < 768 staged -> the region cannot fit before the seam
    auto ctx_r = sg::create_dx12_context({.use_warp = true, .upload_ring_bytes = ring_bytes});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();
    auto& c = static_cast<dx12::dx12_context&>(*ctx);

    constexpr int W = 4, H = 3, N = W * H; // R32_FLOAT: 16-byte tight row -> 256 padded; 3 rows -> 768 staged
    auto tex = c.create_dx12_texture(copy_desc(sg::pixel_format::r32_float, W, H), sg::allocation_info{});
    REQUIRE(tex.has_value());

    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) + 0.5f;

    ctx->advance_epoch_and_wait_for_idle(); // drain, then park the ring cursor just before the seam
    c._upload_inline.debug_set_cursor(cc::u64(park));
    auto const before = c._upload_inline.debug_cursor();
    REQUIRE(ring_bytes - cc::isize(before.next_pos % cc::u64(ring_bytes)) < 768); // setup forces a seam wrap

    // The staged region straddles the seam; a byte-exact round-trip below proves the split copy is correct.
    auto up = ctx->create_command_list();
    up->upload.bytes_to_texture(tex.value(), cc::as_bytes(cc::span<float const>(src, N)));
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_texture(tex.value());
    ctx->submit_command_list(cc::move(down));
    auto const bytes = ctx->wait_for(fut);
    REQUIRE(bytes.has_value());
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - inline texture download splits across the ring seam")
{
    constexpr cc::isize ring_bytes = 2048; // >= tight 768 + padded 256 + 512 alignment slack
    constexpr cc::isize park = ring_bytes - 512;
    auto ctx_r = sg::create_dx12_context({.use_warp = true, .download_ring_bytes = ring_bytes});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();
    auto& c = static_cast<dx12::dx12_context&>(*ctx);

    constexpr int W = 4, H = 3, N = W * H;
    auto tex = c.create_dx12_texture(copy_desc(sg::pixel_format::r32_float, W, H), sg::allocation_info{});
    REQUIRE(tex.has_value());

    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) * 3.0f + 0.25f;

    auto up = ctx->create_command_list(); // seed through the default upload ring (no wrap)
    up->upload.bytes_to_texture(tex.value(), cc::as_bytes(cc::span<float const>(src, N)));
    ctx->submit_command_list(cc::move(up));

    ctx->advance_epoch_and_wait_for_idle(); // drain, then park the readback ring cursor just before the seam
    c._download_inline.debug_set_cursor(cc::u64(park));
    auto const before = c._download_inline.debug_cursor();
    REQUIRE(ring_bytes - cc::isize(before.next_pos % cc::u64(ring_bytes)) < 768); // setup forces a seam wrap

    auto down = ctx->create_command_list();
    auto fut = down->download.bytes_from_texture(tex.value()); // the seam-straddling readback splits here
    ctx->submit_command_list(cc::move(down));

    auto const bytes = ctx->wait_for(fut);
    REQUIRE(bytes.has_value());
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - async texture copy splits across staging windows")
{
    // Tiny 512-byte async windows: each holds exactly one 256-padded row, so an 8x8 R32_FLOAT (8 rows)
    // is packed across several windows on both the upload and readback copy queues.
    auto ctx_r = sg::create_dx12_context(
        {.use_warp = true, .async_upload_window_bytes = 512, .async_download_window_bytes = 512});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();

    constexpr int W = 8, H = 8, N = W * H;
    auto tex = ctx->persistent.create_raw_texture(copy_desc(sg::pixel_format::r32_float, W, H));
    REQUIRE(tex != nullptr);

    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) - 0.5f;

    ctx->upload.bytes_to_texture(tex, cc::make_pinned_data(cc::as_bytes(cc::span<float const>(src, N))));
    auto fut = ctx->download.bytes_from_texture(tex);

    auto const bytes = ctx->wait_for(fut);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(sizeof(src)));
    auto const* got = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - an empty texture region is a no-op")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    constexpr int W = 4, H = 4, N = W * H;
    auto tex = c.persistent.create_raw_texture(copy_desc(sg::pixel_format::r32_float, W, H));
    REQUIRE(tex != nullptr);

    float src[N];
    for (int i = 0; i < N; ++i)
        src[i] = float(i) + 0.5f;

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    // Seed the whole subresource (no region), then an empty-region upload (zero size) — a no-op that needs
    // no pixels and must not disturb the data.
    cmd->upload.bytes_to_texture(tex, cc::as_bytes(cc::span<float const>(src, N)));
    cmd->upload.bytes_to_texture(tex, {}, {},
                                 sg::texture_region{.offset = tg::pos3i(1, 1, 0), .size = tg::vec3i(0, 0, 0)});
    // An empty-region readback returns a ready, empty future; a no-region readback returns the whole subresource.
    auto empty_fut = cmd->download.bytes_from_texture(
        tex, {}, sg::texture_region{.offset = tg::pos3i(0, 0, 0), .size = tg::vec3i(0, 2, 1)});
    auto full_fut = cmd->download.bytes_from_texture(tex);
    c.submit_command_list(cc::move(cmd));

    auto const empty_bytes = c.wait_for(empty_fut);
    REQUIRE(empty_bytes.has_value());
    CHECK(empty_bytes.value().size() == 0);

    auto const full = c.wait_for(full_fut);
    REQUIRE(full.has_value());
    auto const* got = reinterpret_cast<float const*>(full.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != src[i])
            ok = false;
    CHECK(ok); // the empty-region upload left the seeded data intact
}

TEST("sg dx12 - block-compressed texture round-trips whole blocks")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    // 8×8 BC1 = 2×2 blocks of 8 bytes each = 32 bytes; a tight block-row is 16 bytes, staged at 256.
    auto tex = c.persistent.create_raw_texture(copy_desc(sg::pixel_format::bc1_rgba_unorm, 8, 8));
    REQUIRE(tex != nullptr);

    cc::byte src[32];
    for (int i = 0; i < 32; ++i)
        src[i] = cc::byte(i * 7 + 1);

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_texture(tex, cc::span<cc::byte const>(src, 32));
    auto future = cmd->download.bytes_from_texture(tex);
    c.submit_command_list(cc::move(cmd));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 32);
    bool ok = true;
    for (int i = 0; i < 32; ++i)
        if (bytes.value()[i] != src[i])
            ok = false;
    CHECK(ok);
}
