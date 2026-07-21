#pragma once

#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/impl/imgui_texture_registry.hh>
#include <typed-geometry/linalg/vec.hh>

struct ImDrawData;
struct ImDrawVert;

namespace sr
{
/// The renderer half of a Dear ImGui backend, drawn entirely through sg — no native graphics calls.
///
/// Owns everything with a lifetime: the GPU textures backing imgui's atlas, and this frame's geometry.
/// The draw recording itself is sr::imgui_draw_routine, which owns only shader-derived state. Keeping
/// those apart is what lets the routine stay a plain render routine — a routine rebuilds its state on
/// every shader reload, which would be exactly wrong for resources imgui owns the lifetime of.
///
/// Pair it with sr::imgui_context, which owns the ImGui context and the frame bracket:
///
///     auto imgui = sr::imgui_context::create();
///     auto renderer = sr::imgui_renderer::create(ctx);
///     // per frame, after imgui.end_frame():
///     auto* const draw_data = ImGui::GetDrawData();
///     renderer.prepare(*cmd, draw_data);                       // before the rendering scope
///     {
///         auto pass = cmd->raster.render_to({.color_targets = {backbuffer.preserved()}});
///         renderer.render(*cmd, draw_data, {.target_format = fmt, .target_size = size});
///     }
///
/// Drive it from one thread — prepare() and render() both carry per-frame state.
class imgui_renderer
{
    // construction
public:
    /// Declares the renderer capabilities this class implements on the current ImGui context, so an
    /// ImGui context must already exist. Does no GPU work.
    [[nodiscard]] static imgui_renderer create(sg::context& ctx);

    imgui_renderer() = default;

    // per-frame
public:
    /// Services imgui's texture create / update / destroy requests and uploads this frame's vertices and
    /// indices.
    ///
    /// Must run *before* the caller opens its rendering scope: the geometry upload is recorded inline on
    /// `cmd`, and a copy inside a rendering scope is invalid. Texture uploads take the async copy queue
    /// (ctx.upload) instead, so they need no command list at all.
    ///
    /// `draw_data` is non-const because imgui's 1.92 texture protocol writes back into it — the backend
    /// reports each texture's new id and status on ImTextureData.
    void prepare(sg::command_list& cmd, ImDrawData* draw_data);

    /// Records the draws. A rendering scope must already be open on `cmd`, and prepare() must have run
    /// for this same frame.
    ///
    /// `target_size` is in framebuffer pixels; `target_format` must not be an sRGB format (see
    /// imgui_draw_routine).
    struct target_info
    {
        sg::pixel_format target_format = sg::pixel_format::undefined;
        tg::vec2i target_size;
    };
    void render(sg::command_list& cmd, ImDrawData* draw_data, target_info const& target);

    // queries
public:
    /// Number of live GPU textures backing imgui's atlas. For tests and diagnostics.
    [[nodiscard]] isize live_texture_count() const { return _textures.live_texture_count(); }

    /// Drops every GPU texture and hands imgui's side back, so a later frame rebuilds from scratch.
    /// Useful when the ImGui context outlives this renderer.
    void release_textures(ImDrawData* draw_data) { _textures.release_all(draw_data); }

private:
    impl::imgui_texture_registry _textures;

    // This frame's geometry, transient: the bump allocator resets per epoch, which is exactly a frame's
    // lifetime, so there is no growth policy to get wrong.
    sg::buffer<ImDrawVert> _vertices;
    sg::buffer<u16> _indices;

    /// Set by prepare(), cleared by render(), so a missing or duplicated prepare() is caught rather than
    /// silently drawing last frame's geometry.
    bool _prepared = false;
};
} // namespace sr
