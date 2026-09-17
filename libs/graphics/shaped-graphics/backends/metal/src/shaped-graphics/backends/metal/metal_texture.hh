#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_resource_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg::backend::metal
{
/// Mints the process-unique stamp behind `metal_texture::identity()`.
[[nodiscard]] u64 next_texture_identity();
} // namespace sg::backend::metal

/// Metal implementation of sg::raw_texture.
///
/// One MTLTexture, and — unlike both other backends — no layout state beside it.
/// A Metal texture has no layout at all, so there is nothing here answering the question dx12's and vulkan's texture
/// trackers exist for; `current_texture_layout` answers `general` for every texture and every range.
class sg::backend::metal::metal_texture final : public sg::raw_texture
{
public:
    metal_texture(metal_context& ctx,
                  sg::texture_description const& desc,
                  MTL::Texture* texture,
                  sg::memory_heap_handle heap = nullptr)
      : sg::raw_texture(desc), _ctx(ctx), _texture(texture), _heap(cc::move(heap))
    {
    }

    ~metal_texture() override;

    [[nodiscard]] MTL::Texture* texture() const { return _texture; }

    /// Access tracking, shared by every command list recording against this texture.
    /// The same type buffers use: a Metal texture has no layout, so it has no state a buffer does not also have.
    [[nodiscard]] cc::mutex<metal_resource_access>& access() const { return _access; }

    /// The direct-queue submission that last named this texture; see `submission_stamp`.
    [[nodiscard]] submission_stamp& submission() const { return _submission; }

    /// A stamp no other texture in this process shares, minted at construction.
    ///
    /// **What the view cache keys on, rather than this object's address.**
    /// A transient texture is destroyed and recreated every frame and the allocator hands the new one the dead one's
    /// address, so an address key lets it inherit views of a texture that no longer exists.
    [[nodiscard]] u64 identity() const { return _identity; }

    /// The id an argument buffer names this texture by.
    /// Metal 4 binds a resource id rather than a descriptor, so this is the whole of what a binding writes.
    [[nodiscard]] u64 gpu_resource_id() const { return _texture != nullptr ? _texture->gpuResourceID()._impl : u64(0); }

private:
    void on_expired() const override;
    void release_storage() const;

    metal_context& _ctx;
    mutable MTL::Texture* _texture = nullptr;
    sg::memory_heap_handle _heap;
    mutable cc::mutex<metal_resource_access> _access;
    mutable submission_stamp _submission; // mutable: a list declares against a handle to const
    u64 _identity = next_texture_identity();
};
