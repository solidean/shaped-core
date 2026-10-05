#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-viewer/drawing/annotation.hh>
#include <shaped-viewer/drawing/decal.hh>
#include <shaped-viewer/drawing/instance.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/scene/scene_item.hh>
#include <shaped-viewer/view/render_settings.hh>

/// What a layer draws into the view that holds it.
enum class sv::layer_kind : sv::u8
{
    /// A nested layout tree, rendered into this view's texture — the recursion in the model.
    layout,

    /// The path trace; the only kind that accumulates temporally today.
    scene_3d,

    /// 2D drawings, instanced in the view's logical pixels and drawn over the layers below with no depth.
    /// libs/graphics/shaped-viewer/docs/canvas.md is the design.
    canvas,

    /// Dear ImGui, drawn through sr::imgui_routine.
    ui,
};

/// How a layer combines with what the layers below it already put in the view's texture.
///
/// Every sv view target holds *premultiplied* alpha.
/// Straight alpha is not associative across a nesting chain, so a three-level composite would visibly darken edges.
enum class sv::layer_blend : sv::u8
{
    /// Overwrite — what the bottom layer of a view normally wants, and what leaves the target fully defined.
    replace,

    /// Source over destination, premultiplied.
    over,
};

/// One composited stratum of a view.
///
/// Kind-tagged rather than a variant, so a layer stays a per-frame value that is cheap to rebuild and a plan builder
/// can walk it without allocating.
/// Only the fields this layer's `kind` reads apply.
///
/// Layers composite in the order the view lists them, each over the ones before it.
struct sv::layer
{
    layer_kind kind = layer_kind::scene_3d;
    layer_blend blend = layer_blend::over;
    float opacity = 1.0f;

    /// layout: the sub-tree's root, a node of viewer_definition::nodes.
    layout_node_id root_node = invalid_node;

    /// scene_3d: geometry, lights, the environment a missed ray sees, and the trace's own knobs.
    ///
    /// A traced layer writes no meaningful alpha yet, so it is always opaque and is forced to `replace`.
    /// Compositing one `over` another needs the raygen to write coverage into `.a` first (see libs/graphics/shaped-viewer/docs/TODO.md).
    cc::vector<scene_item> items;
    cc::vector<scene_light> lights; ///< in the caller's order, which the GPU record does not have to keep

    /// Traced in place of `lights` when that is empty, so a layer nobody lit is still visible.
    /// Empty turns it off, and such a layer is lit by its background alone.
    cc::optional<sv::light> fallback_light = default_fallback_light();
    sv::background background;
    render_settings settings;

    /// scene_3d and canvas: the drawings instanced on this layer, in the order they were added.
    /// A scene's are 3D, in world units, drawn after its trace; a canvas's are 2D, in the view's logical pixels.
    cc::vector<drawing_placement> drawings;

    /// scene_3d: labels flat on screen at points of the scene, placed every frame through the view's camera.
    cc::vector<annotation_record> annotations;

    /// scene_3d: drawings projected onto the traced surfaces, painted at every hit in the order they were added.
    cc::vector<decal_placement> decals;

    /// scene_3d: whether anything drawn after the trace needs its primary-hit depth.
    [[nodiscard]] bool needs_primary_depth() const { return !drawings.empty() || !annotations.empty(); }

    /// ui: which of the frame's registered draw callbacks fills this layer.
    u32 ui_callback = u32(-1);
};
