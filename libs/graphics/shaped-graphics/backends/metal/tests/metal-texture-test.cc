#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/backends/metal/metal_texture_view_cache.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/views.hh>

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

    CHECK(mtl::texture_usage_of(sg::texture_usage::texture) == MTL::TextureUsageShaderRead);
    CHECK(mtl::texture_usage_of(sg::texture_usage::render_target) == MTL::TextureUsageRenderTarget);

    // An image is readable too — Metal spells the two bits separately where sg has one usage.
    CHECK(mtl::texture_usage_of(sg::texture_usage::image) == (MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite));
}

ASYNC_TEST("sg metal - a texture round-trips through inline transfer")
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

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == source.size());

    auto mismatches = 0;
    for (auto i = 0; i < source.size(); ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of {} bytes differ", mismatches, source.size()));
}

TEST("sg metal - a view's texture type comes from the view, not the texture")
{
    // A one-face view of a cube is a 2D texture: asking Metal for a one-slice Cube is a combination it refuses, and
    // the texture's own type is the wrong answer for every view that reshapes it.
    using sg::texture_view_dimension;

    CHECK(mtl::texture_type_of(texture_view_dimension::tex_1d) == MTL::TextureType1D);
    CHECK(mtl::texture_type_of(texture_view_dimension::tex_1d_array) == MTL::TextureType1DArray);
    CHECK(mtl::texture_type_of(texture_view_dimension::tex_2d) == MTL::TextureType2D);
    CHECK(mtl::texture_type_of(texture_view_dimension::tex_2d_ms) == MTL::TextureType2DMultisample);
    CHECK(mtl::texture_type_of(texture_view_dimension::tex_2d_array) == MTL::TextureType2DArray);
    CHECK(mtl::texture_type_of(texture_view_dimension::tex_2d_ms_array) == MTL::TextureType2DMultisampleArray);
    CHECK(mtl::texture_type_of(texture_view_dimension::tex_3d) == MTL::TextureType3D);
    CHECK(mtl::texture_type_of(texture_view_dimension::cube) == MTL::TextureTypeCube);
    CHECK(mtl::texture_type_of(texture_view_dimension::cube_array) == MTL::TextureTypeCubeArray);
}

TEST("sg metal - a one-face view of a cube is a 2D texture")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto texture = ctx->persistent.create_raw_texture({.format = sg::pixel_format::rgba8_unorm,
                                                       .width = 32,
                                                       .height = 32,
                                                       .mip_levels = 1,
                                                       .is_cube = true,
                                                       .usage = sg::texture_usage::texture});
    REQUIRE(texture != nullptr);

    // Face 2 alone, which is where reusing the texture's own type asks for a one-slice Cube.
    auto* const face = ctx->texture_views().acquire(
        {.bound_as = sg::view_class::texture,
         .texture = texture,
         .view_dimension = sg::texture_view_dimension::tex_2d,
         .range = {{.start = 0, .end = 1}, {.start = 2, .end = 3}, {.start = 0, .end = 1}}});
    REQUIRE(face != nullptr);
    CHECK(face->textureType() == MTL::TextureType2D);

    // And the whole cube, in its own format and shape, is the texture itself rather than a minted view.
    auto* const whole = ctx->texture_views().acquire(
        {.bound_as = sg::view_class::texture,
         .texture = texture,
         .view_dimension = sg::texture_view_dimension::cube,
         .range = {{.start = 0, .end = 1}, {.start = 0, .end = 6}, {.start = 0, .end = 1}}});
    CHECK(whole == static_cast<mtl::metal_texture const&>(*texture).texture());
}

ASYNC_TEST("sg metal - a cached view is evicted with its texture")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // A view retains its parent, so an entry that outlives its texture keeps that MTLTexture alive for the context's
    // whole lifetime — and hands the next texture at that address a view of an object that no longer exists.
    auto const before = ctx->texture_views().debug_entry_count();

    {
        auto texture = ctx->persistent.create_raw_texture({.format = sg::pixel_format::rgba8_unorm,
                                                           .width = 32,
                                                           .height = 32,
                                                           .mip_levels = 4,
                                                           .usage = sg::texture_usage::texture});
        REQUIRE(texture != nullptr);

        auto* const mip = ctx->texture_views().acquire(
            {.bound_as = sg::view_class::texture,
             .texture = texture,
             .view_dimension = sg::texture_view_dimension::tex_2d,
             .range = {{.start = 1, .end = 2}, {.start = 0, .end = 1}, {.start = 0, .end = 1}}});
        REQUIRE(mip != nullptr);
        CHECK(ctx->texture_views().debug_entry_count() == before + 1);
    }

    // The eviction rides the texture's finalizer, which runs where its MTLTexture is released: at epoch retire.
    ctx->advance_epoch();
    co_await ctx->idle_completion();

    CHECK(ctx->texture_views().debug_entry_count() == before);
}
