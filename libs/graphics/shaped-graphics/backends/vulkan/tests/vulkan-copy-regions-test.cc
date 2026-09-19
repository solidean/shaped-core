#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture_copy_regions.hh>

using namespace cc::primitive_defines;
using sg::backend::vulkan::build_texture_copy_regions;
using sg::backend::vulkan::max_copy_regions;

// How a run of streamed rows becomes copy regions — the pure half of the async transfer paths, needing no device.
//
// A row is a BLOCK row on a compressed format and rows run slice-major on a 3D one, so a run of rows is up to three
// boxes; getting either conversion wrong fills part of a texture silently.

namespace
{
constexpr auto color = VkImageSubresourceLayers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1};

struct regions
{
    VkBufferImageCopy copies[max_copy_regions] = {};
    int count = 0;
};

[[nodiscard]] regions build(sg::texture_region const& region,
                            sg::pixel_format format,
                            isize first_row,
                            isize row_count,
                            isize row_bytes,
                            VkDeviceSize base_offset = 0)
{
    auto r = regions();
    r.count = build_texture_copy_regions(region, format, first_row, row_count, row_bytes, base_offset, color,
                                         cc::span<VkBufferImageCopy>(r.copies));
    return r;
}
} // namespace

TEST("vulkan copy regions - a whole 2D texture is one region")
{
    auto const r = build({.size = tg::vec3i(8, 8, 1)}, sg::pixel_format::rgba8_unorm, 0, 8, 32, 256);

    REQUIRE(r.count == 1);
    CHECK(r.copies[0].bufferOffset == 256);
    CHECK(r.copies[0].imageOffset.y == 0);
    CHECK(r.copies[0].imageExtent.width == 8);
    CHECK(r.copies[0].imageExtent.height == 8);
    CHECK(r.copies[0].imageExtent.depth == 1);
}

TEST("vulkan copy regions - a compressed row is four texel rows")
{
    // 16 texels high is four block rows; rows 1 and 2 are texels 4 through 11.
    auto const r = build({.size = tg::vec3i(16, 16, 1)}, sg::pixel_format::bc1_rgba_unorm, 1, 2, 32);

    REQUIRE(r.count == 1);
    CHECK(r.copies[0].imageOffset.y == 4);
    CHECK(r.copies[0].imageExtent.height == 8);
}

TEST("vulkan copy regions - the last block row of an uneven height is clamped to the image")
{
    // Six texels high is two block rows, the second covering only texels 4 and 5.
    // An extent reaching past the image is a validation error rather than a clipped copy.
    auto const r = build({.size = tg::vec3i(8, 6, 1)}, sg::pixel_format::bc1_rgba_unorm, 1, 1, 16);

    REQUIRE(r.count == 1);
    CHECK(r.copies[0].imageOffset.y == 4);
    CHECK(r.copies[0].imageExtent.height == 2);
}

TEST("vulkan copy regions - a run across 3D slices is a partial slice, whole slices and a partial slice")
{
    // Four rows per slice; rows 2..12 are the last two rows of slice 0, all of slices 1 and 2, and row 0 of slice 3.
    isize const row_bytes = 16;
    auto const r = build({.size = tg::vec3i(4, 4, 4)}, sg::pixel_format::rgba8_unorm, 2, 11, row_bytes, 1000);

    REQUIRE(r.count == 3);

    CHECK(r.copies[0].bufferOffset == 1000);
    CHECK(r.copies[0].imageOffset.z == 0);
    CHECK(r.copies[0].imageOffset.y == 2);
    CHECK(r.copies[0].imageExtent.height == 2);
    CHECK(r.copies[0].imageExtent.depth == 1);

    CHECK(r.copies[1].bufferOffset == VkDeviceSize(1000 + 2 * row_bytes));
    CHECK(r.copies[1].imageOffset.z == 1);
    CHECK(r.copies[1].imageOffset.y == 0);
    CHECK(r.copies[1].imageExtent.height == 4);
    CHECK(r.copies[1].imageExtent.depth == 2);

    CHECK(r.copies[2].bufferOffset == VkDeviceSize(1000 + 10 * row_bytes));
    CHECK(r.copies[2].imageOffset.z == 3);
    CHECK(r.copies[2].imageOffset.y == 0);
    CHECK(r.copies[2].imageExtent.height == 1);
    CHECK(r.copies[2].imageExtent.depth == 1);
}

TEST("vulkan copy regions - no rows is no regions")
{
    CHECK(build({.size = tg::vec3i(8, 8, 1)}, sg::pixel_format::rgba8_unorm, 0, 0, 32).count == 0);
}
