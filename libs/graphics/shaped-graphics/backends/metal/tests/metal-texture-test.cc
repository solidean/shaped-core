#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>

// Textures: creation, the format and shape translation, and a round trip through inline transfer.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

TEST("sg metal - every sg pixel format maps to a metal one")
{
    // Device-free: the mapping is a switch, and sg's format set is already the intersection every realistic backend
    // supports — so an unmapped format means one was added without being translated, not that a caller asked for
    // something exotic.
    CHECK(mtl::pixel_format_of(sg::pixel_format::undefined) == MTL::PixelFormatInvalid);

    for (auto i = 1; i <= int(sg::pixel_format::bc7_rgba_unorm_srgb); ++i)
    {
        auto const format = sg::pixel_format(i);
        CHECK(mtl::pixel_format_of(format) != MTL::PixelFormatInvalid)
            .context(cc::format("pixel_format {} has no metal equivalent", i));
    }
}

TEST("sg metal - a texture shape folds into one MTLTextureType")
{
    // Metal folds dimension, array-ness, cube-ness and multisampling into one enum where sg keeps them orthogonal, so
    // the product is worth pinning — each of these is a combination a reader cannot infer from the enum.
    using sg::texture_dimension;

    CHECK(mtl::texture_type_of(texture_dimension::d1, false, false, false) == MTL::TextureType1D);
    CHECK(mtl::texture_type_of(texture_dimension::d1, true, false, false) == MTL::TextureType1DArray);
    CHECK(mtl::texture_type_of(texture_dimension::d2, false, false, false) == MTL::TextureType2D);
    CHECK(mtl::texture_type_of(texture_dimension::d2, true, false, false) == MTL::TextureType2DArray);
    CHECK(mtl::texture_type_of(texture_dimension::d2, false, false, true) == MTL::TextureType2DMultisample);
    CHECK(mtl::texture_type_of(texture_dimension::d2, true, false, true) == MTL::TextureType2DMultisampleArray);
    CHECK(mtl::texture_type_of(texture_dimension::d3, false, false, false) == MTL::TextureType3D);

    // Cube-ness wins over the dimension, and a cube array counts cubes rather than faces.
    CHECK(mtl::texture_type_of(texture_dimension::d2, false, true, false) == MTL::TextureTypeCube);
    CHECK(mtl::texture_type_of(texture_dimension::d2, true, true, false) == MTL::TextureTypeCubeArray);
}

TEST("sg metal - copy usage needs no metal bit")
{
    // Metal has no copy usage at all: every texture can be a copy source and destination, the way it is implicit on
    // D3D12.
    // So the two sg flags that say so map to nothing, and a copy-only texture asks for no usage bits.
    CHECK(mtl::texture_usage_of(sg::texture_usage::copy_src | sg::texture_usage::copy_dst) == MTL::TextureUsageUnknown);

    CHECK(mtl::texture_usage_of(sg::texture_usage::readonly_texture) == MTL::TextureUsageShaderRead);
    CHECK(mtl::texture_usage_of(sg::texture_usage::render_target) == MTL::TextureUsageRenderTarget);

    // A read-write texture is readable too — Metal spells the two bits separately where sg has one usage.
    CHECK(mtl::texture_usage_of(sg::texture_usage::readwrite_texture)
          == (MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite));
}

TEST("sg metal - a texture round-trips through inline transfer")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    constexpr auto k_size = 8;
    auto const texture = ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });

    auto source = cc::vector<byte>::create_uninitialized(k_size * k_size * 4);
    for (auto i = 0; i < source.size(); ++i)
        source[i] = byte(u8((i * 13 + 7) & 0xFF));

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(texture.raw(), source);
    auto future = cmd->download.bytes_from_texture(texture.raw());
    ctx->submit_command_list(cc::move(cmd));

    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == source.size());

    auto mismatches = 0;
    for (auto i = 0; i < source.size(); ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of {} bytes differ", mismatches, source.size()));
}
