#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/thread/mutex.hh>
#include <imgui/imgui_fwd.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/impl/imgui_texture_registry.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sr
{
/// Renders one frame of Dear ImGui draw data through sg — the renderer half of an imgui backend.
/// Everything goes through sg; there are no native graphics calls.
///
/// Pair it with sr::imgui_context, which owns the ImGui context and the frame bracket:
///
///     auto imgui = sr::imgui_context::create();
///     // per frame, after imgui.end_frame():
///     auto pass = cmd->raster.render_to({.color_targets = {backbuffer.preserved()}});
///     sr::imgui_routine::execute(pass, ImGui::GetDrawData());
///
/// The routine owns the GPU textures backing imgui's atlas and a pipeline per target format.
/// All of it lives behind one mutex, taken for the length of each entry point, so two threads recording imgui against the same context serialize rather than race.
/// The atlas deliberately survives a shader reload — it has nothing to do with our shaders.
///
/// This frame's geometry is deliberately *not* state:
/// it is allocated from the transient scope and lives on the stack for one execute(), so the call is re-entrant across imgui's viewports.
///
/// `draw_data` is non-const because imgui's 1.92 texture protocol writes back into it:
/// the backend reports each texture's new id and status on ImTextureData.
class imgui_routine : public sg::render_routine<imgui_routine>
{
    // per-frame
public:
    /// Draws one frame of imgui into an open rendering scope:
    /// services its texture create / update / destroy requests, uploads this frame's geometry, and records the draws.
    /// Pass the `scope` that `cmd.raster.render_to(...)` returned — the target's color format and extent are read from it.
    ///
    /// The scope's color format must not be an sRGB format:
    /// imgui's vertex colors and font atlas are already sRGB-encoded 8-bit values, and a `*_unorm_srgb` target would encode them a second time.
    /// Bind a non-srgb view of the same resource instead.
    /// Compensating in the shader would cost a conversion per pixel to undo something the caller did not ask for.
    static void execute(sg::rendering_scope& scope, ImDrawData* draw_data);

    /// Draws and presents every imgui viewport except the main one, each into its own swapchain.
    ///
    /// Call it once per frame after sr::imgui_context::update_viewports, and — this part matters — *before* the main window's present rather than after.
    /// Moving an OS window and presenting its new content are not synchronized, so between the two the window shows content drawn for where it used to be.
    /// A main present that blocks on vsync in between stretches that gap to a full frame, and the contents of a window being dragged visibly lag the window.
    ///
    /// A viewport's swapchain is created here on first sight and follows its window's size by itself.
    /// Unlike execute() this opens its own rendering scopes and submits its own command lists, so it must NOT be called inside one.
    ///
    /// A no-op while multi-viewport is off.
    /// Viewport swapchains use a bgra8_unorm target, independent of whatever format the caller's main swapchain has.
    static void render_viewports(sg::context& ctx);

protected:
    void init_declare(sg::context& ctx) override;

private:
    struct pipeline_entry
    {
        sg::pixel_format format = sg::pixel_format::undefined;
        sg::raster_pipeline_handle pipeline;
    };

    /// Everything this routine mutates, in one place so the locking rule is checkable by inspection.
    /// The shader-derived half is rebuilt by init_declare on every reload; the atlas is not.
    struct state
    {
        sg::binding_group_layout_handle group_layout;
        sg::pipeline_layout_handle pipeline_layout;
        sg::compiled_shader vertex_shader;
        sg::compiled_shader fragment_shader;

        /// One pipeline per color-target format drawn to — in practice exactly one, the swapchain's.
        /// TODO(sg): fold into ctx.cached.acquire_raster_pipeline once pipeline_cache grows a graphics tier.
        /// Its key already covers target formats, so this and the blocking build both go away.
        cc::small_vector<pipeline_entry, 2> pipelines;

        impl::imgui_texture_registry textures;
    };

    cc::mutex<state> _state;

    /// One viewport's draw data, concatenated into a single buffer pair.
    /// Lives on the stack for the length of one execute() — under multi-viewport that call runs once per viewport per frame,
    /// and caching this on the routine would have each viewport overwrite the previous one's geometry.
    struct geometry
    {
        sg::buffer<ImDrawVert> vertices;
        sg::buffer<u16> indices;
    };

    /// Assumes `s` is already locked.
    /// Builds (or finds) the pipeline for `format`.
    [[nodiscard]] static sg::raster_pipeline const* pipeline_for(state& s, sg::context& ctx, sg::pixel_format format);

    /// Allocates this frame's transient vertex + index buffers and records their inline uploads.
    /// The buffers are epoch-scoped: the returned handles do not own them, and they expire with the frame.
    [[nodiscard]] static geometry upload_geometry(sg::command_list& cmd, ImDrawData* draw_data);
};

/// Renders one finished imgui frame to `main` and to every secondary viewport —
/// the batteries-included path for when imgui *owns* the window rather than being composited over the caller's own rendering.
///
/// Call it once per frame after imgui.end_frame().
/// It runs the whole render half in the order that matters:
/// draws this frame's main viewport into `main`'s backbuffer (cleared to `clear_color`), updates and draws the secondary viewport windows, then presents `main` last.
///
/// This owns `main`'s present, which is exactly why it cannot composite imgui over your own scene —
/// for that, open your own rendering scope and call imgui_routine::execute(scope, ...) directly, then update_viewports + render_viewports yourself.
/// This is a thin facade over those primitives, not a replacement.
///
/// The caller still drives input and the frame bracket (poll_events / process_events / begin_frame / the UI / end_frame) and skips a minimized window —
/// `main` must have a drawable backbuffer here.
void render_imgui(imgui_context& imgui,
                  sg::context& ctx,
                  sg::swapchain& main,
                  tg::vec4f clear_color = tg::vec4f(0, 0, 0, 1));
} // namespace sr
