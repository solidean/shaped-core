#include "dx12-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

using namespace cc::primitive_defines;

// A resource someone else created, as an sg texture.
// The debug layer is what checks the state at each submit: a resource sg does not find in COMMON fails it, and that
// message fails the test through the log rule.

namespace
{
namespace dx12 = sg::backend::dx12;

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

/// A render-target resource created outside sg, through the legacy API, in COMMON.
dx12::ComPtr<ID3D12Resource> make_foreign_target(dx12::dx12_context& ctx, int width)
{
    auto const heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_DEFAULT};
    auto const desc = D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = UINT64(width),
        .Height = UINT(k_size),
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {.Count = 1},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    auto resource = dx12::ComPtr<ID3D12Resource>();
    auto const hr = ctx.device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    REQUIRE(SUCCEEDED(hr));
    return resource;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg dx12 - a wrapped foreign resource renders, reads back and is handed back in its layout",
                     (dx12::dx12_context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const foreign = make_foreign_target(*ctx, k_size);

    auto wrapped = ctx->wrap_dx12_texture(foreign.Get(), target_desc());
    REQUIRE(wrapped.has_value());
    auto const tex = sg::raw_texture_handle(wrapped.value());
    auto const typed = sg::texture_2d::from_raw(tex);

    auto cmd = ctx->create_command_list();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(tg::vec4f(0, 1, 0, 1))}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    // Back in COMMON for its owner, which the next submit's interop check also wants.
    cmd->ensure_layout(tex, sg::texture_layout::general);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == isize(k_size) * k_size * 4);
    auto const* px = reinterpret_cast<u8 const*>(bytes.data());
    auto all_green = true;
    for (auto i = 0; i < k_size * k_size; ++i)
        if (px[i * 4 + 0] != 0 || px[i * 4 + 1] != 255 || px[i * 4 + 2] != 0 || px[i * 4 + 3] != 255)
            all_green = false;
    CHECK(all_green);
    CHECK(ctx->current_texture_layout(tex, sg::subresource_range{}) == sg::texture_layout::general);
}

INVOCABLE_TEST("sg dx12 - wrapping refuses a resource whose shape is not the description's",
               (dx12::dx12_context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const foreign = make_foreign_target(*ctx, k_size * 2);

    auto const wrapped = ctx->wrap_dx12_texture(foreign.Get(), target_desc());
    REQUIRE(wrapped.has_error());
    CHECK(wrapped.error().to_string().contains("32x16x1"));
}
