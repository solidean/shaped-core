#pragma once

#include <clean-core/common/utility.hh> // cc::as_bytes
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/index_buffer_view.hh>
#include <shaped-graphics/vertex_buffer_view.hh>
#include <shaped-graphics/views.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

#include <initializer_list>
#include <type_traits>

/// Raster (graphics-pipeline) recording: bind a set of color / depth-stencil targets as a rendering scope and, per target, clear / preserve / discard its contents.
/// Reached as `cmd.raster`.
/// Draw support arrives with the graphics pipeline; for now a scope only applies its begin-ops (clear/discard).

namespace sg
{
/// What happens to a target's contents at the start of a rendering scope.
/// Set through the view's .cleared() / .preserved() / .discarded() members, not by hand.
enum class target_op : cc::u8
{
    preserve, ///< keep the existing contents
    clear,    ///< clear to a value before rendering
    discard,  ///< contents become undefined (neither loaded nor cleared)
};

/// A color (render-target) target of a rendering scope: the view plus what to do with it at pass start.
/// Build with view.cleared(color) / view.preserved() / view.discarded().
struct color_target
{
    render_target_view view;
    target_op op = target_op::preserve;
    tg::vec4f clear_color; ///< used only when op == clear
};

/// A depth-stencil target of a rendering scope.
/// Build with view.cleared(depth[, stencil]) / view.preserved() / view.discarded().
struct depth_stencil_target
{
    depth_stencil_view view;
    target_op op = target_op::preserve;
    float clear_depth = 1.0f; ///< used only when op == clear
    cc::u8 clear_stencil = 0; ///< used only when op == clear
};

/// The region of the target(s) rendering maps to, plus the depth range.
/// Offset/size are in pixels.
/// Omitting the viewport from rendering_info uses the full target extent with depth [0, 1].
struct viewport
{
    tg::pos2f offset; ///< top-left, in pixels
    tg::vec2f size;   ///< width / height, in pixels
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

/// The targets + rasterizer state a rendering scope binds — passed to cmd.raster.render_to / cmd.raster.manual.begin_rendering.
/// viewport / scissor unset default to the full target extent.
struct rendering_info
{
    cc::fixed_vector<color_target, max_color_targets> color_targets;
    cc::optional<sg::depth_stencil_target> depth_stencil_target;
    cc::optional<sg::viewport> viewport;
    cc::optional<tg::aabb2i> scissor; ///< pixel rect; unset => full target extent
};

/// Parameters of a non-indexed draw: a `{offset = first vertex, size = vertex count}` vertex range,
/// drawn once per instance in the `{offset = first instance, size = instance count}` instance range.
struct draw_config
{
    cc::offset_size vertex_range = {.offset = 0, .size = 0};
    cc::offset_size instance_range = {.offset = 0, .size = 1};
};

/// Parameters of an indexed draw: an `{offset = first index, size = index count}` index range into the bound index buffer,
/// each index offset by `vertex_offset` before the vertex fetch,
/// drawn once per instance in the `{offset = first instance, size = instance count}` instance range.
struct draw_indexed_config
{
    cc::offset_size index_range = {.offset = 0, .size = 0};
    cc::offset_size instance_range = {.offset = 0, .size = 1};
    int vertex_offset = 0; ///< added to each index before the vertex fetch (sub-mesh base vertex)
};

/// RAII handle for an open rendering scope, returned by cmd.raster.render_to.
/// Opens the scope on construction (begin_rendering) and closes it at end of scope (end_rendering).
///
/// The raster draw recording lives on this handle: bind a pipeline, set viewport / scissor / inline constants, draw.
/// The same calls are also on `cmd.raster` (both forward to the one command list),
/// but recording through the scope keeps the "draw into this pass" flow on the object that opened it,
/// and lets a routine handed only the scope record without a separate command_list argument.
///
/// Only raster operations are here.
/// Anything else the command list offers — uploads, downloads, the context — is reached through `command_list()`; a rendering scope does not mirror them.
class rendering_scope
{
public:
    ~rendering_scope();

    // Pinned to its command list: neither copyable nor movable (render_to returns it by mandatory copy-elision, so no move is needed).
    rendering_scope(rendering_scope const&) = delete;
    rendering_scope(rendering_scope&&) = delete;
    rendering_scope& operator=(rendering_scope const&) = delete;
    rendering_scope& operator=(rendering_scope&&) = delete;

    // scope queries

    /// The command list this scope records into — for the non-raster operations a scope does not mirror (`command_list().context()`, `command_list().upload`, …).
    /// (`class command_list` disambiguates the type from this accessor of the same name.)
    [[nodiscard]] class command_list& command_list() const { return _cmd; }

    /// Pixel extent the scope's targets share — the size to drive a viewport, scissor or projection with.
    [[nodiscard]] tg::vec2i render_target_size() const { return _size; }

    /// Formats of the bound color targets, in order — usually one.
    /// A routine's pipeline bakes these in, so they are what to build (or key) that pipeline against.
    [[nodiscard]] cc::span<pixel_format const> color_formats() const { return _color_formats; }

    /// Format of the bound depth-stencil target, or empty when the scope has none attached.
    [[nodiscard]] cc::optional<pixel_format> depth_format() const { return _depth_format; }

    // raster draw recording — valid while this scope is alive.
    // Identical to the same calls on cmd.raster; both forward to the owning command list.

    /// Binds `pipeline` as the active raster pipeline for subsequent bind_group / draw calls.
    void bind_pipeline(raster_pipeline const& pipeline);
    /// Binds `group` to descriptor set `set` of the active pipeline's layout (must match that slot).
    void bind_group(int set, binding_group const& group);
    /// Binds vertex buffers to consecutive input slots starting at `first_slot` (slot first_slot+i <- views[i]).
    void bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot = 0);
    void bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot = 0);
    void bind_vertex_buffer(vertex_buffer_view const& view, int slot = 0);
    /// Binds the index buffer read by draw_indexed.
    void bind_index_buffer(index_buffer_view const& view);
    /// Overrides the rendering scope's viewport / scissor for subsequent draws.
    void set_viewport(viewport const& vp);
    void set_scissor(tg::aabb2i const& rect);
    /// Sets the stencil reference the depth-stencil state's stencil test compares against.
    void set_stencil_reference(u32 reference);
    /// Sets the constant RGBA blend factor that referencing factors use.
    void set_blend_constants(tg::vec4f constants);
    /// Writes inline constants into the bound pipeline layout's inline_constants block.
    void set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset = {});
    /// POD convenience: bit-copies `value`. `T` must be trivially copyable, size a multiple of 4 bytes.
    template <class T>
    void set_inline_constants(T const& value, cc::optional<cc::isize> offset = {})
    {
        static_assert(std::is_trivially_copyable_v<T>, "inline-constants payload must be trivially copyable");
        static_assert(sizeof(T) % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");
        set_inline_constants(cc::as_bytes(cc::span<T const>(&value, 1)), offset);
    }
    /// Records a non-indexed draw of the active pipeline.
    void draw(draw_config const& config = {});
    /// Records an indexed draw of the active pipeline (an index buffer must be bound).
    void draw_indexed(draw_indexed_config const& config = {});

private:
    friend class command_list_raster_scope;
    rendering_scope(class command_list& cmd, rendering_info const& info); // begins rendering with `info`

    class command_list& _cmd;
    tg::vec2i _size;
    cc::fixed_vector<pixel_format, max_color_targets> _color_formats;
    cc::optional<pixel_format> _depth_format;
};

/// Low-level rendering passthrough, reached as cmd.raster.manual: begin / end a rendering scope by hand, forwarding straight to the backend.
/// begin_rendering and end_rendering must be balanced.
/// Prefer render_to, which pairs them via RAII.
class command_list_raster_manual_scope
{
public:
    void begin_rendering(rendering_info const& info);
    void end_rendering();

    // Draw recording — valid only while a rendering scope is open (begin_rendering / render_to).
    // The same API is on cmd.raster; both forward to the owning command list.

    /// Binds `pipeline` as the active raster pipeline for subsequent bind_group / draw calls.
    void bind_pipeline(raster_pipeline const& pipeline);
    /// Binds `group` to descriptor set `set` of the active pipeline's layout (must match that slot).
    void bind_group(int set, binding_group const& group);
    /// Binds vertex buffers to consecutive input slots starting at `first_slot` (slot first_slot+i <- views[i]).
    void bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot = 0);
    void bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot = 0);
    void bind_vertex_buffer(vertex_buffer_view const& view, int slot = 0);
    /// Binds the index buffer read by draw_indexed.
    void bind_index_buffer(index_buffer_view const& view);
    /// Overrides the rendering scope's viewport / scissor for subsequent draws.
    void set_viewport(viewport const& vp);
    void set_scissor(tg::aabb2i const& rect);
    /// Sets the stencil reference the depth-stencil state's stencil test compares against.
    void set_stencil_reference(u32 reference);
    /// Sets the constant RGBA factor blend factors that reference it use.
    void set_blend_constants(tg::vec4f constants);
    /// Writes inline constants into the bound pipeline layout's inline_constants block (see cmd.compute).
    void set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset = {});
    /// POD convenience: bit-copies `value`. `T` must be trivially copyable, size a multiple of 4 bytes.
    template <class T>
    void set_inline_constants(T const& value, cc::optional<cc::isize> offset = {})
    {
        static_assert(std::is_trivially_copyable_v<T>, "inline-constants payload must be trivially copyable");
        static_assert(sizeof(T) % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");
        set_inline_constants(cc::as_bytes(cc::span<T const>(&value, 1)), offset);
    }
    /// Records a non-indexed draw of the active pipeline.
    void draw(draw_config const& config = {});
    /// Records an indexed draw of the active pipeline (an index buffer must be bound).
    void draw_indexed(draw_indexed_config const& config = {});

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

/// Raster recording facade for a command list, reached as `cmd.raster`: open a rendering scope over a set of targets, clearing / preserving / discarding each.
/// `manual` exposes the same begin/end by hand.
class command_list_raster_scope
{
public:
    /// Opens a rendering scope over `info`'s targets (applying each target's clear / discard) and returns an RAII handle;
    /// rendering ends when the returned scope is destroyed.
    [[nodiscard]] rendering_scope render_to(rendering_info const& info);

    /// Low-level passthrough: begin / end a rendering scope by hand. Prefer render_to.
    command_list_raster_manual_scope manual;

    // Draw recording — valid only while a rendering scope is open (render_to keeps one alive; or manual.begin_rendering).
    // The same API is on cmd.raster.manual; both forward to the command list.

    /// Binds `pipeline` as the active raster pipeline for subsequent bind_group / draw calls.
    void bind_pipeline(raster_pipeline const& pipeline);
    /// Binds `group` to descriptor set `set` of the active pipeline's layout (must match that slot).
    void bind_group(int set, binding_group const& group);
    /// Binds vertex buffers to consecutive input slots starting at `first_slot` (slot first_slot+i <- views[i]).
    void bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot = 0);
    void bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot = 0);
    void bind_vertex_buffer(vertex_buffer_view const& view, int slot = 0);
    /// Binds the index buffer read by draw_indexed.
    void bind_index_buffer(index_buffer_view const& view);
    /// Overrides the rendering scope's viewport / scissor for subsequent draws.
    void set_viewport(viewport const& vp);
    void set_scissor(tg::aabb2i const& rect);
    /// Sets the stencil reference the depth-stencil state's stencil test compares against.
    void set_stencil_reference(u32 reference);
    /// Sets the constant RGBA factor blend factors that reference it use.
    void set_blend_constants(tg::vec4f constants);
    /// Writes inline constants into the bound pipeline layout's inline_constants block (see cmd.compute).
    void set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset = {});
    /// POD convenience: bit-copies `value`. `T` must be trivially copyable, size a multiple of 4 bytes.
    template <class T>
    void set_inline_constants(T const& value, cc::optional<cc::isize> offset = {})
    {
        static_assert(std::is_trivially_copyable_v<T>, "inline-constants payload must be trivially copyable");
        static_assert(sizeof(T) % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");
        set_inline_constants(cc::as_bytes(cc::span<T const>(&value, 1)), offset);
    }
    /// Records a non-indexed draw of the active pipeline.
    void draw(draw_config const& config = {});
    /// Records an indexed draw of the active pipeline (an index buffer must be bound).
    void draw_indexed(draw_indexed_config const& config = {});

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
