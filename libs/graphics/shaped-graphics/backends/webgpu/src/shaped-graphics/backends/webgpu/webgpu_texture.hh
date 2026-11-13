#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/subresource.hh>
#include <shaped-graphics/resource/views.hh>

/// A WebGPU texture together with the views asked of it.
///
/// Shared rather than owned by one handle, because a transient texture asked for again in the next epoch is the same
/// WebGPU texture under a new handle, and it brings its views along.
/// **A browser frees a view only when its garbage collector runs**, so a view made per bind is what this exists to stop.
struct sg::backend::webgpu::webgpu_texture_storage
{
    wgpu_texture texture;

    /// Identifies the WebGPU texture for as long as it exists, across every handle it passes through.
    /// What a reused binding group is keyed on, since a freed texture's pointer can come back as another one.
    u64 serial = 0;

    struct cached_view
    {
        sg::texture_view_dimension dimension = {};
        sg::pixel_format format = sg::pixel_format::undefined;
        sg::subresource_range range;
        wgpu_texture_view view;

        /// The epoch it was last asked for; a view over less than the whole texture goes once it is two epochs old.
        u64 last_used_epoch = 0;

        /// Over the whole texture in its own format: kept for the texture's lifetime, since nearly every bind asks for it.
        bool whole = false;
    };
    cc::vector<cached_view> views;

    /// Whether the context's sweep already visits this storage for its partial views.
    bool swept = false;
};

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
    webgpu_texture(webgpu_context& ctx,
                   sg::texture_description const& desc,
                   std::shared_ptr<webgpu_texture_storage> storage,
                   bool owned);
    ~webgpu_texture() override;

    /// The WebGPU texture, null once expired.
    [[nodiscard]] WGPUTexture raw() const { return _storage != nullptr ? _storage->texture.get() : nullptr; }

    /// The WebGPU texture's serial (see webgpu_texture_storage::serial); 0 once expired.
    [[nodiscard]] u64 serial() const { return _storage != nullptr ? _storage->serial : 0; }

    /// A view over `range` read as `dimension` in `format`, which is the texture's own format when undefined.
    /// Cached on the texture: the same request in a later epoch hands back the same WebGPU view.
    [[nodiscard]] wgpu_texture_view create_view(sg::texture_view_dimension dimension,
                                                sg::pixel_format format,
                                                sg::subresource_range const& range) const;

    /// Marks this handle transient, so its WebGPU texture is offered for reuse when it expires rather than destroyed.
    void mark_transient() { _transient = true; }

    void release_cached_view(sg::texture_view_dimension dimension,
                             sg::pixel_format format,
                             sg::subresource_range const& range) const override;
    void release_cached_views() const override;

protected:
    void on_expired() const override;

private:
    void release_storage() const;

    webgpu_context& _ctx;
    sg::context::live_resource _live; // counts this resource alive against _ctx, construction to destruction
    mutable std::shared_ptr<webgpu_texture_storage> _storage; // mutable: expiry is a const lifetime hook
    bool _owned = true;
    bool _transient = false;
};

namespace sg::backend::webgpu
{
/// The formats a texture of `format` may also be viewed as, which WebGPU fixes at creation: its sRGB twin, where it has one.
[[nodiscard]] cc::span<WGPUTextureFormat const> view_formats_of(sg::pixel_format format);
} // namespace sg::backend::webgpu
