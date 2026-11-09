#include "metal-test-common.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

// An MTLTexture someone else created, as an sg texture: Compositor Services' drawable is the case it exists for.

namespace
{
namespace mtl = sg::backend::metal;

constexpr int k_size = 16;

sg::texture_description target_desc()
{
    auto d = sg::texture_description();
    d.format = sg::pixel_format::rgba8_unorm;
    d.dimension = sg::texture_dimension::d2;
    d.width = k_size;
    d.height = k_size;
    d.usage = sg::texture_usage::render_target | sg::texture_usage::copy_src;
    return d;
}

/// A texture created outside sg, as another framework hands one over.
MTL::Texture* make_foreign_texture(mtl::metal_context& ctx, int width, MTL::TextureUsage usage)
{
    auto* const descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA8Unorm, NS::UInteger(width), NS::UInteger(k_size), false);
    descriptor->setUsage(usage);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    return ctx.device()->newTexture(descriptor);
}
} // namespace

ASYNC_TEST("sg metal - a wrapped foreign texture renders and reads back, and outlives its owner's release")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto* const foreign = make_foreign_texture(*ctx, k_size, MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    REQUIRE(foreign != nullptr);
    auto wrapped = ctx->wrap_metal_texture(foreign, target_desc());
    REQUIRE(wrapped.has_value()).context(wrapped.has_error() ? wrapped.error().to_string() : cc::string());
    // the owner lets go at once, and the texture is sg's to keep alive for as long as it is used
    foreign->release();

    auto const tex = sg::raw_texture_handle(wrapped.value());
    auto const typed = sg::texture_2d::from_raw(tex);
    auto cmd = ctx->create_command_list();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(tg::vec4f(0, 1, 0, 1))}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == isize(k_size) * k_size * 4);
    auto const* px = reinterpret_cast<u8 const*>(bytes.data());
    auto all_green = true;
    for (auto i = 0; i < k_size * k_size; ++i)
        if (px[i * 4 + 0] != 0 || px[i * 4 + 1] != 255 || px[i * 4 + 2] != 0 || px[i * 4 + 3] != 255)
            all_green = false;
    CHECK(all_green);
}

TEST("sg metal - wrapping refuses a texture whose shape is not the description's, or which lacks a usage it needs")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto* const wide = make_foreign_texture(*ctx, k_size * 2, MTL::TextureUsageRenderTarget);
    auto const misshapen = ctx->wrap_metal_texture(wide, target_desc());
    REQUIRE(misshapen.has_error());
    CHECK(misshapen.error().to_string().contains("32x16x1")).context(misshapen.error().to_string());
    wide->release();

    // drawn into as a render target, which a texture made for sampling alone cannot be
    auto* const sampled = make_foreign_texture(*ctx, k_size, MTL::TextureUsageShaderRead);
    auto const unusable = ctx->wrap_metal_texture(sampled, target_desc());
    REQUIRE(unusable.has_error());
    CHECK(unusable.error().to_string().contains("usage")).context(unusable.error().to_string());
    sampled->release();
}
