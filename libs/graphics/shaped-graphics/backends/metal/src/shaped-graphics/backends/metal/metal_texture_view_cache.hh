#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

/// What a cached view object is keyed by: the texture's identity stamp, plus every field that reaches
/// `newTextureView`.
///
/// **Not `hash(sg::raw_texture_view)`**, however well that reads: sg's view hash folds `texture.get()`, the address,
/// and an address is recycled the moment a per-frame texture is released.
/// The vulkan build-out shipped that bug and found it in a frame loop rather than in its suite, because the failure
/// needs an allocator to hand a new texture a dead one's address.
struct sg::backend::metal::metal_texture_view_key
{
    u64 texture_identity = 0;
    view_class access = view_class::readonly;
    texture_view_dimension dimension = texture_view_dimension::tex_2d;
    pixel_format format = pixel_format::undefined;
    subresource_range range;
    cc::start_end depth_slice_range = {.start = 0, .end = 0};

    [[nodiscard]] friend bool operator==(metal_texture_view_key const&, metal_texture_view_key const&) = default;

    /// Folds exactly what `operator==` compares; defaulting the operator is what keeps the two agreeing as fields are
    /// added.
    [[nodiscard]] friend u64 hash(metal_texture_view_key const& k)
    {
        return cc::make_hash(k.texture_identity, k.access, k.dimension, k.format, k.range, k.depth_slice_range.start,
                             k.depth_slice_range.end);
    }
};

/// MTLTextures for bound texture views, keyed by the view's own identity and shared context-wide.
///
/// **Per context rather than per group**, for the same reason samplers are.
/// A dx12 texture view is a descriptor written straight into a heap and leaves no object behind; Metal needs a texture
/// view object that outlives every argument buffer naming it, and giving each group its own would mean deferring its
/// destruction behind that group's epoch.
///
/// **An entry is evicted with its texture, and that is load-bearing rather than tidy.**
/// A view retains its parent, so an entry that outlives the texture keeps the MTLTexture alive for the context's whole
/// lifetime — every per-frame texture ever bound, forever.
/// The eviction rides the texture's own finalizer, so it runs exactly where the MTLTexture is released.
class sg::backend::metal::metal_texture_view_cache
{
public:
    /// The view object for `view`, minted on first use.
    /// A view that names the whole texture in its own format and shape needs no object at all and returns the texture
    /// itself; null means the texture has no storage, or Metal refused the view.
    [[nodiscard]] MTL::Texture* acquire(sg::raw_texture_view const& view);

    /// Drops every cached view.
    /// Called at shutdown, before the device goes.
    void shutdown();

    /// How many view objects are cached right now.
    /// Test-only: the leak this cache's eviction exists to prevent is invisible from the outside until VRAM is gone.
    [[nodiscard]] isize debug_entry_count();

private:
    using view_map = cc::map<metal_texture_view_key, MTL::Texture*>;

    /// Registers the eviction of `key` on `texture`'s own finalizer list.
    /// `this` outlives every texture: the cache belongs to the context, and a texture cannot survive it.
    void forget_with_texture(sg::raw_texture const& texture, metal_texture_view_key key);

    /// The view objects this cache minted, keyed by view identity.
    cc::mutex<view_map> _views;
};
