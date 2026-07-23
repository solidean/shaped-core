#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <imgui/imgui.h> // ImTextureID / ImTextureData / ImDrawData — this is a backend-internal header
#include <shaped-graphics/texture.hh>
#include <shaped-rendering/fwd.hh>

namespace sr::impl
{
/// Maps imgui's ImTextureID onto sg textures, and services the create / update / destroy requests imgui raises on ImTextureData each frame.
///
/// This is the part of the renderer that imgui 1.92 made non-trivial: the font atlas is no longer built once and uploaded once.
/// Glyphs are rasterized on demand, so the atlas grows and is patched at runtime,
/// and a backend must set ImGuiBackendFlags_RendererHasTextures and drain ImDrawData::Textures every frame.
/// Nothing here is optional — skipping a request wedges imgui's atlas.
///
/// Uploads go through ctx.upload (the async copy queue), not a command list: a font atlas is bulk asset data,
/// and a later command list that samples the texture waits on the copy automatically.
/// That is why service_requests needs only a context.
///
/// The ID handed to imgui is `slot index + 1`, so 0 stays ImTextureID_Invalid and a zero-initialized ImTextureData is never mistaken for slot 0.
/// Freed slots are recycled, so IDs are not monotonic — imgui never assumes they are.
///
///     registry.service_requests(ctx, draw_data);          // once per frame, before recording draws
///     auto const* texture = registry.try_texture_of(cmd.GetTexID());
class imgui_texture_registry
{
    // frame protocol
public:
    /// Creates, updates and destroys GPU textures to match what imgui is asking for this frame.
    /// Must run before the draws that sample them are recorded.
    void service_requests(sg::context& ctx, ImDrawData* draw_data);

    // queries
public:
    /// The texture behind an ImDrawCmd's id, or an error if imgui named one the registry never created.
    /// A copy of the handle — cheap (shared owner), and it cannot dangle the way a pointer into _slots could.
    /// (Not `cc::result<texture_2d const>`: result constructs its value with placement new, which a const type rejects.
    /// The value is the caller's own copy to const as it likes.)
    [[nodiscard]] cc::result<sg::texture_2d> try_texture_of(ImTextureID id) const;

private:
    void create_texture(sg::context& ctx, ImTextureData* tex);
    void update_texture(sg::context& ctx, ImTextureData* tex);
    void destroy_texture(ImTextureData* tex);

    /// Slots are addressed by `id - 1`; a null texture is a free slot.
    cc::vector<sg::texture_2d> _slots;
    cc::vector<isize> _free_slots;
};
} // namespace sr::impl
