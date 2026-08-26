#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/view/camera.hh>
#include <shaped-viewer/view/layer.hh>
#include <shaped-viewer/view/view_id.hh>
#include <typed-geometry/linalg/vec.hh>

/// A resource a view keeps across frames, and the rule that invalidates it.
///
/// A temporal accumulator reset on camera movement is the motivating case, and the path tracer's own accumulation is
/// exactly one of these rather than a case baked into the renderer.
///
/// `reset_hash` must cover exactly what makes the history stale — and nothing else.
/// Where the view sits, how it is fitted, sampled or throttled must never reach it, or a slider drag would throw away
/// a converged image.
struct sv::temporal_input
{
    /// Names this resource within its view; a layer derives it from its own index, so two layers cannot collide.
    /// `sv::temporal_id` mints the ones the renderer implies — a caller declaring their own must not collide with those.
    u64 id = 0;

    /// Unset means the view's own resolution, which is what an accumulator wants.
    cc::optional<tg::vec2i> resolution;

    sg::pixel_format format = sg::pixel_format::rgba16_float;

    u64 reset_hash = 0;
};

namespace sv::temporal_id
{
/// The ids the renderer mints for a traced layer, keyed by the layer's own index so two layers cannot collide.
///
/// The high half of the id is the *kind* and the low half the layer, so a kind added later needs no renumbering.
/// A caller declaring a `temporal_input` of their own keeps out of the reserved range by leaving the high half clear.
inline constexpr u64 kind_shift = 32;
inline constexpr u64 caller_range_end = u64(1) << kind_shift;

/// Where a traced layer blends its running estimate — the path tracer's accumulator.
[[nodiscard]] constexpr u64 accumulation(u8 layer)
{
    return (u64(1) << kind_shift) | u64(layer);
}

/// Whether `id` is an accumulation slot, whatever layer it belongs to.
///
/// For a caller folding over every traced layer of a view rather than naming one.
/// Asking about layer 0 is the mistake this exists to prevent: a view whose first layer is a layout or a ui overlay
/// has its trace at index 1, and `accumulation(0)` then names a slot that does not exist.
[[nodiscard]] constexpr bool is_accumulation(u64 id)
{
    return (id >> kind_shift) == 1;
}
} // namespace sv::temporal_id

/// How often a view re-renders, as a fraction of the frame loop's rate.
///
/// 1 is every frame, 0.5 every second frame, 0 only when something invalidates it.
/// A struct rather than a bare float, so smarter policies (converge-then-idle, invalidate-driven) land later without
/// breaking callers.
struct sv::refresh_policy
{
    float rate = 1.0f;
};

/// One texture a frame renders, and everything that goes into it.
///
/// A view is the *definition of a texture*, so a layer that is itself a layout tree renders that whole sub-tree into
/// this view's texture — the recursion in the model is exactly texture nesting.
///
/// A view is a plain per-frame value: build a fresh one each frame.
/// Its `id` is the only part that persists, naming the temporal resources and the composite target it reuses.
///
/// Deliberately absent: where the view goes.
/// Placement belongs to the leaf that references it, which is what makes "relayout must not restart a converged
/// image" a property of the type rather than a rule a comment has to keep asking for.
struct sv::view_data
{
    view_id id;

    /// Always a concrete pixel size.
    /// `resolution_follows_layout` means the referencing leaf's rect writes it during the layout solve; otherwise it
    /// is the caller's, and the leaf's fit mode reconciles it against the rect it landed in.
    tg::vec2i resolution = tg::vec2i(0, 0);
    bool resolution_follows_layout = true;

    /// The eye this view is drawn from.
    sv::camera camera;

    /// Composited in order, each over the ones before it.
    cc::vector<layer> layers;

    refresh_policy refresh = {};

    /// Resources this view keeps across frames.
    cc::vector<temporal_input> temporal_inputs;
};

namespace sv
{
/// The view's first `scene_3d` layer, or nullptr when it has none.
/// The trace binds one 3D layer per view today; several is the multi-layer seam.
[[nodiscard]] layer const* primary_scene_3d(view_data const& v);

/// The view's first `scene_3d` layer, appending one if it has none.
/// This is what an authoring call that adds a mesh or a light reaches for.
[[nodiscard]] layer& ensure_scene_3d(view_data& v);

/// Whether `l` holds anything a trace could actually render.
///
/// A `scene_3d` layer carrying only lights or only a background is legitimate authoring — `add_scene().add_light(...)`
/// is the obvious way to light a view before its geometry exists — and it must render an empty image rather than
/// reaching the renderer, which asserts that a trace has geometry to bind.
/// The plan and the temporal inputs both key off this, so a layer that traces nothing also allocates nothing.
[[nodiscard]] bool is_traceable(layer const& l);

/// Every temporal resource `v` needs this frame: the ones it declared, plus one accumulator per traced layer.
///
/// The tracer's accumulator is *derived* rather than baked into the renderer, which is what makes it one temporal
/// input among others instead of a special case — the plan sizes it and the store keeps it exactly like any other.
/// A resolution left unset is still unset here; only the plan knows what the view settled on.
[[nodiscard]] cc::vector<temporal_input> temporal_inputs_of(view_data const& v);
} // namespace sv
