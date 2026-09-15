#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

/// MTLTextures for bound texture views, keyed by the view's own identity and shared context-wide.
///
/// **Per context rather than per group**, for the same reason samplers are.
/// A dx12 texture view is a descriptor written straight into a heap and leaves no object behind; Metal needs a texture
/// view object that outlives every argument buffer naming it, and giving each group its own would mean deferring its
/// destruction behind that group's epoch.
///
/// **Keyed on `hash(raw_texture_view)` — sg's own identity for the view — rather than on the texture's address.**
/// That distinction is not pedantry: a per-frame texture's address is recycled, so an address-keyed cache hands a new
/// texture the previous one's view, of an object that no longer exists.
/// The vulkan build-out shipped exactly that bug and found it in a frame loop rather than in its suite, because the
/// failure needs an allocator to reuse an address.
class sg::backend::metal::metal_texture_view_cache
{
public:
    /// The view object for `view`, minted on first use.
    /// A view that names the whole texture in its own format needs no object at all and returns the texture itself.
    [[nodiscard]] MTL::Texture* acquire(sg::raw_texture_view const& view);

    /// Drops every cached view.
    /// Called at shutdown, before the device goes.
    void shutdown();

private:
    /// The view objects this cache minted, keyed by sg view identity.
    /// A value may be null, which is the "no view object needed" answer cached rather than recomputed.
    cc::mutex<cc::map<u64, MTL::Texture*>> _views;
};
