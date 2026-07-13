#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/attachment_views.hh>
#include <shaped-graphics/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// Raster (graphics-pipeline) recording: bind a set of color / depth-stencil targets as a rendering
/// scope and, per target, clear / keep / discard its contents. Reached as `cmd.raster`. Draw support
/// arrives with the graphics pipeline; for now a scope only applies its begin-ops (clear/discard).

namespace sg
{
/// What happens to a target's contents at the start of a rendering scope. Set through the
/// sg::clear / sg::keep / sg::discard factories, not by hand.
enum class target_op
{
    keep,    ///< preserve the existing contents
    clear,   ///< clear to a value before rendering
    discard, ///< contents become undefined (neither loaded nor cleared)
};

/// A color (render-target) target of a rendering scope: the view plus what to do with it at pass start.
/// Build with sg::clear(view, color) / sg::keep(view) / sg::discard(view).
struct color_target
{
    render_target_view view;
    target_op op = target_op::keep;
    tg::vec4f clear_color; ///< used only when op == clear
};

/// A depth-stencil target of a rendering scope. Build with sg::clear(view, depth[, stencil]) /
/// sg::keep(view) / sg::discard(view).
struct depth_stencil_target
{
    depth_stencil_view view;
    target_op op = target_op::keep;
    float clear_depth = 1.0f; ///< used only when op == clear
    cc::u8 clear_stencil = 0; ///< used only when op == clear
};

// Target factories — the op is the function name, so no op enum appears at the call site.

[[nodiscard]] color_target clear(render_target_view view, tg::vec4f color);
[[nodiscard]] color_target keep(render_target_view view);
[[nodiscard]] color_target discard(render_target_view view);

[[nodiscard]] depth_stencil_target clear(depth_stencil_view view, float depth, cc::u8 stencil = 0);
[[nodiscard]] depth_stencil_target keep(depth_stencil_view view);
[[nodiscard]] depth_stencil_target discard(depth_stencil_view view);

/// The region of the target(s) rendering maps to, plus the depth range. Offset/size are in pixels.
/// Omitting the viewport from rendering_info uses the full target extent with depth [0, 1].
struct viewport
{
    tg::pos2f offset; ///< top-left, in pixels
    tg::vec2f size;   ///< width / height, in pixels
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

/// The targets + rasterizer state a rendering scope binds — passed to cmd.raster.render_to /
/// cmd.raster.manual.begin_rendering. viewport / scissor unset default to the full target extent.
struct rendering_info
{
    cc::small_vector<color_target, 8> color_targets;
    cc::optional<sg::depth_stencil_target> depth_stencil_target;
    cc::optional<sg::viewport> viewport;
    cc::optional<tg::aabb2i> scissor; ///< pixel rect; unset => full target extent
};

/// RAII handle for an open rendering scope, returned by cmd.raster.render_to. Opens the scope on
/// construction (begin_rendering) and closes it at end of scope (end_rendering). Draw calls arrive with
/// the graphics pipeline.
class rendering_scope
{
public:
    ~rendering_scope();

    // Pinned to its command list: neither copyable nor movable (render_to returns it by mandatory
    // copy-elision, so no move is needed).
    rendering_scope(rendering_scope const&) = delete;
    rendering_scope(rendering_scope&&) = delete;
    rendering_scope& operator=(rendering_scope const&) = delete;
    rendering_scope& operator=(rendering_scope&&) = delete;

private:
    friend class command_list_raster_scope;
    rendering_scope(command_list& cmd, rendering_info const& info); // begins rendering with `info`

    command_list& _cmd;
};

/// Low-level rendering passthrough, reached as cmd.raster.manual: begin / end a rendering scope by hand,
/// forwarding straight to the backend. begin_rendering and end_rendering must be balanced. Prefer
/// render_to, which pairs them via RAII.
class command_list_raster_manual_scope
{
public:
    void begin_rendering(rendering_info const& info);
    void end_rendering();

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_raster_manual_scope(command_list_raster_manual_scope const&) = delete;
    command_list_raster_manual_scope(command_list_raster_manual_scope&&) = delete;
    command_list_raster_manual_scope& operator=(command_list_raster_manual_scope const&) = delete;
    command_list_raster_manual_scope& operator=(command_list_raster_manual_scope&&) = delete;

private:
    friend class command_list_raster_scope;
    explicit command_list_raster_manual_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};

/// Raster recording facade for a command list, reached as `cmd.raster`: open a rendering scope over a
/// set of targets, clearing / keeping / discarding each. `manual` exposes the same begin/end by hand.
class command_list_raster_scope
{
public:
    /// Opens a rendering scope over `info`'s targets (applying each target's clear / discard) and
    /// returns an RAII handle; rendering ends when the returned scope is destroyed.
    [[nodiscard]] rendering_scope render_to(rendering_info const& info);

    /// Low-level passthrough: begin / end a rendering scope by hand. Prefer render_to.
    command_list_raster_manual_scope manual;

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_raster_scope(command_list_raster_scope const&) = delete;
    command_list_raster_scope(command_list_raster_scope&&) = delete;
    command_list_raster_scope& operator=(command_list_raster_scope const&) = delete;
    command_list_raster_scope& operator=(command_list_raster_scope&&) = delete;

private:
    friend class command_list;
    explicit command_list_raster_scope(command_list& cmd) : manual(cmd), _cmd(cmd) {}

    command_list& _cmd;
};
} // namespace sg
