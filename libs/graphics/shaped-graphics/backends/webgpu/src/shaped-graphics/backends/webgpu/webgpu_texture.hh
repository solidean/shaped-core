#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/subresource.hh>
#include <shaped-graphics/resource/views.hh>

/// WebGPU implementation of sg::raw_texture.
///
/// Every sg 1D texture is a WebGPU 2D texture of height 1, and a 1D view a 2D one: WebGPU's own 1D textures allow no mips, no arrays and no storage or render use.
/// A shader reading one declares `texture_2d` and addresses row 0 — see libs/graphics/shaped-graphics/backends/webgpu/docs/wgsl.md.
///
/// WebGPU tracks usage and synchronization itself, so a texture here carries no layout state: every layout sg asks about is `general`.
class sg::backend::webgpu::webgpu_texture final : public sg::raw_texture
{
public:
    /// `owned` is false for a texture that belongs to a surface, which only its surface may destroy.
    webgpu_texture(webgpu_context& ctx, sg::texture_description const& desc, wgpu_texture texture, bool owned);
    ~webgpu_texture() override;

    /// The WebGPU texture, null once expired.
    [[nodiscard]] WGPUTexture raw() const { return _texture.get(); }

    /// A view over `range` read as `dimension` in `format`, which is the texture's own format when undefined.
    [[nodiscard]] wgpu_texture_view create_view(sg::texture_view_dimension dimension,
                                                sg::pixel_format format,
                                                sg::subresource_range const& range) const;

protected:
    void on_expired() const override;

private:
    void release_storage() const;

    webgpu_context& _ctx;
    mutable wgpu_texture _texture; // mutable: expiry is a const lifetime hook
    bool _owned = true;
};

namespace sg::backend::webgpu
{
/// The formats a texture of `format` may also be viewed as, which WebGPU fixes at creation: its sRGB twin, where it has one.
[[nodiscard]] cc::span<WGPUTextureFormat const> view_formats_of(sg::pixel_format format);
} // namespace sg::backend::webgpu
