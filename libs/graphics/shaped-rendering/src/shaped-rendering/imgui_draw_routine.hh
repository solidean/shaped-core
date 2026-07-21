#pragma once

#include <clean-core/container/small_vector.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/impl/imgui_texture_registry.hh>
#include <typed-geometry/linalg/vec.hh>

struct ImDrawData;
struct ImDrawVert;

namespace sr
{
/// Records one frame of Dear ImGui draw data through sg — the draw half of the imgui backend.
///
/// Owns only what derives from the imgui shaders: the binding group layout, the pipeline layout, and one
/// pipeline per target format. Everything with a lifetime — the atlas textures, the geometry — belongs to
/// sr::imgui_renderer and arrives through `params`. That split is deliberate: a render routine rebuilds
/// its state whenever shaders reload, which is exactly wrong for GPU resources imgui owns the lifetime of.
///
/// Call it through sr::imgui_renderer rather than directly; the renderer is what guarantees the textures
/// and geometry in `params` were prepared for this frame.
class imgui_draw_routine : public sg::render_routine<imgui_draw_routine>
{
    // parameters
public:
    struct params
    {
        /// Color format of the target the caller's rendering scope is bound to — the pipeline bakes it in.
        /// Must not be an sRGB format; see the note on execute().
        sg::pixel_format target_format = sg::pixel_format::undefined;

        /// Target extent in framebuffer pixels. Scissor rects are clamped to it.
        tg::vec2i target_size;

        /// This frame's geometry, already uploaded.
        sg::buffer<ImDrawVert> const& vertices;
        sg::buffer<u16> const& indices;

        /// Resolves each draw command's texture id. Must already have been serviced for this frame.
        impl::imgui_texture_registry const& textures;
    };

    // execution
public:
    /// Records the draws for `draw_data`. A rendering scope must already be open on `cmd`.
    ///
    /// The target must not be an sRGB format: imgui's vertex colors and font atlas are already
    /// sRGB-encoded 8-bit values, and a `*_unorm_srgb` target would encode them a second time. Bind a
    /// non-srgb view of the same resource instead. Compensating in the shader would cost a conversion per
    /// pixel to undo something the caller did not want.
    ///
    /// `draw_data` is non-const because imgui's own accessors are; nothing here mutates it.
    static void execute(sg::command_list& cmd, ImDrawData* draw_data, params const& p);

protected:
    void init_declare(sg::context& ctx) override;

private:
    [[nodiscard]] sg::raster_pipeline const* pipeline_for(sg::context& ctx, sg::pixel_format format) const;

    sg::binding_group_layout_handle _group_layout;
    sg::pipeline_layout_handle _pipeline_layout;
    sg::compiled_shader _vertex_shader;
    sg::compiled_shader _fragment_shader;

    struct pipeline_entry
    {
        sg::pixel_format format = sg::pixel_format::undefined;
        sg::raster_pipeline_handle pipeline;
    };

    /// One pipeline per color-target format drawn to — in practice exactly one, the swapchain's, so the
    /// linear scan is a couple of compares. Mutable because this is memoization: a pure function of
    /// (pipeline layout, shaders, format), with no lifetime of its own beyond the routine's.
    ///
    /// TODO(sg): fold into ctx.cached.acquire_raster_pipeline once pipeline_cache grows a graphics tier
    /// (see the TODO in pipeline_cache.hh). Its key already covers target formats, so this member and the
    /// blocking build in pipeline_for both go away.
    mutable cc::small_vector<pipeline_entry, 2> _pipelines;
};
} // namespace sr
