#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// End-to-end attachment views on WARP: a texture's as_render_target_view() / as_depth_stencil_view()
// becomes a real D3D12 RTV / DSV descriptor in the context's non-shader-visible heaps. The WARP debug
// layer is the oracle — it rejects a malformed desc, so a returned descriptor_ref means it was accepted.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::texture_description tex_desc(sg::texture_usage usage, sg::pixel_format format, int layers = 0)
{
    sg::texture_description d;
    d.format = format;
    d.dimension = sg::texture_dimension::d2;
    d.width = 128;
    d.height = 128;
    d.usage = usage;
    if (layers > 0)
        d.array_layers = layers;
    return d;
}
} // namespace

TEST("sg dx12 - render-target views create valid RTV descriptors")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // whole-texture 2D color target
    {
        auto tex = c.create_dx12_texture(tex_desc(sg::texture_usage::render_target, sg::pixel_format::rgba8_unorm),
                                         sg::allocation_info{});
        REQUIRE(tex.has_value());
        sg::texture_2d const typed(tex.value());
        auto rtv = c.create_dx12_render_target_view(typed.as_render_target_view());
        REQUIRE(rtv.has_value()); // CreateRenderTargetView succeeded + the debug layer accepted it
        CHECK(rtv.value().slot != dx12::cpu_descriptor_slot::invalid);
        c.free_dx12_render_target_view(rtv.value().slot);
    }

    // one array slice bound as a Texture2D RTV
    {
        auto tex = c.create_dx12_texture(tex_desc(sg::texture_usage::render_target, sg::pixel_format::rgba8_unorm, 4),
                                         sg::allocation_info{});
        REQUIRE(tex.has_value());
        sg::texture_2d_array const typed(tex.value());
        auto rtv = c.create_dx12_render_target_view(typed.as_render_target_2d_view({.slice = 2}));
        REQUIRE(rtv.has_value());
        c.free_dx12_render_target_view(rtv.value().slot);
    }
}

TEST("sg dx12 - depth-stencil views create valid DSV descriptors")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // depth-only
    {
        auto tex = c.create_dx12_texture(tex_desc(sg::texture_usage::depth_stencil, sg::pixel_format::depth32_float),
                                         sg::allocation_info{});
        REQUIRE(tex.has_value());
        sg::texture_2d const typed(tex.value());
        auto dsv = c.create_dx12_depth_stencil_view(typed.as_depth_stencil_view());
        REQUIRE(dsv.has_value()); // CreateDepthStencilView succeeded + the debug layer accepted it
        c.free_dx12_depth_stencil_view(dsv.value().slot);
    }

    // combined depth + stencil
    {
        auto tex = c.create_dx12_texture(
            tex_desc(sg::texture_usage::depth_stencil, sg::pixel_format::depth32_float_stencil8), sg::allocation_info{});
        REQUIRE(tex.has_value());
        sg::texture_2d const typed(tex.value());
        auto dsv = c.create_dx12_depth_stencil_view(typed.as_depth_stencil_view());
        REQUIRE(dsv.has_value());
        c.free_dx12_depth_stencil_view(dsv.value().slot);
    }
}
