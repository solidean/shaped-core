#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/view/layer.hh>
#include <shaped-viewer/view/post_process.hh>
#include <shaped-viewer/view/view_id.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// What a single draw of a target's pass puts down.
///
/// `background` and `border` are both flat-color fills and share one pipeline; they stay distinct enumerators because a
/// plan is read as a description of the frame, where "the node's fill" and "one band of its frame" are different things.
enum class sv::draw_kind : sv::u8
{
    background, ///< a node's whole border box, in a flat color
    border,     ///< one band of a node's border ring, in a flat color
    view,       ///< one view's texture across a rect
    wipe,       ///< two views split along an axis — the leaf's post-process
};

/// Whether a draw samples a finished target or a trace's own accumulation texture.
enum class sv::draw_source_kind : sv::u8
{
    target,
    trace,
};

/// Why a view's subtree could not be planned.
/// The frame still renders: the offending leaf simply loses its source, and every sibling is unaffected.
enum class sv::diagnostic_reason : sv::u8
{
    cycle,                 ///< a view's layout layer reaches back to the view itself
    too_deep,              ///< nesting past max_layout_depth
    missing_view,          ///< a leaf names a view index that does not exist
    unsupported_chain,     ///< a post-process chain beyond its first stage, which wants an intra-frame texture pool
    source_count_mismatch, ///< the leaf's view count disagrees with what its post-process combines
};

namespace sv
{

/// How deep layout-tree layers may nest before the builder stops descending.
/// A cap rather than a stack budget: the recursion is shallow by construction, and a definition past this is authored
/// wrongly rather than merely large.
inline constexpr int max_layout_depth = 16;

/// Ceiling on how many textures one frame may plan, so a pathological definition degrades instead of allocating without bound.
inline constexpr int max_plan_targets = 256;

/// The "no region" sentinel for an index into `render_plan::hit_regions`.
/// Distinct from `invalid_node`: a region names the layout node it came from, but is not itself one.
inline constexpr u32 invalid_hit_region = u32(-1);
} // namespace sv

/// One texture a draw reads, and which part of it.
struct sv::draw_source
{
    draw_source_kind kind = draw_source_kind::target;
    u32 index = 0; ///< into render_plan::targets or render_plan::traces

    /// The sampled sub-rect, normalized.
    /// This is where the leaf's fit mode and its zoom have already been resolved, so nothing downstream knows what a fit mode is.
    tg::aabb2f uv = tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1));
};

/// One draw of one target's pass, in the order the layout resolved it.
struct sv::layout_draw
{
    draw_kind kind = draw_kind::view;

    /// In the target's own pixels — the pass's viewport and scissor.
    tg::aabb2i dst_rect = {};

    /// background and border only.
    tg::vec4f color = tg::vec4f(0, 0, 0, 0);

    draw_source primary = {};
    draw_source secondary = {}; ///< wipe only

    sampler_mode sampler = sampler_mode::linear;
    layer_blend blend = layer_blend::over;
    float opacity = 1.0f;
    post_process post = {};

    /// The layout node this came from; `invalid_node` for a draw a layer emitted directly.
    layout_node_id node = invalid_node;
};

/// One texture the frame writes: a view's composite target, or the frame's output.
struct sv::plan_target
{
    view_id id;
    view_index view = view_index(0);

    tg::vec2i resolution = tg::vec2i(0, 0);
    sg::pixel_format format = sg::pixel_format::rgba16_float;

    /// The caller's own color target rather than one of ours: nothing allocates it, and nothing may keep its content.
    bool is_output = false;

    /// False means this frame records nothing into it, and whatever samples it reads the previous frame's content.
    /// Only ever false for a target that already holds a valid image — a first sighting or a resize always refreshes.
    bool refresh = true;
};

/// One 3D-scene layer resolved to a dispatch writing its own accumulation texture.
struct sv::plan_trace
{
    view_id id;
    view_index view = view_index(0);
    u8 layer = 0; ///< which of the view's layers, so several traced layers cannot share one accumulator

    tg::vec2i resolution = tg::vec2i(0, 0);
};

/// One leaf mapped all the way up into window space, so a cursor can be routed into a nested view.
///
/// Fit modes only ever produce an axis-aligned affine map, so `scale` and `offset` are enough:
/// a window point `w` lands on view texel `(w - offset) / scale`.
struct sv::hit_region
{
    view_index view = view_index(0);
    view_id id; ///< that view's identity, so a hit can be routed without the definition it came from
    layout_node_id node = invalid_node;

    tg::aabb2i window_rect = {};
    tg::vec2f scale = tg::vec2f(1, 1);
    tg::vec2f offset = tg::vec2f(0, 0);

    /// The region this one sits inside, or `invalid_hit_region` at the top level.
    /// An index into `render_plan::hit_regions`, not a layout node — a region is per *reference*, so two of them can
    /// share one node.
    ///
    /// Depth alone cannot order a hit test and neither can a flat counter: a shallow overlay drawn late belongs in
    /// front of a deep view drawn early.
    /// What settles it is the *path* through these links, compared at the first ancestor the two candidates disagree
    /// on — which is exactly painter's order.
    /// `pick_hit_region` does that.
    u32 parent = invalid_hit_region;

    /// Emission order, which is draw order *among siblings* only.
    /// Comparing it across different branches is what makes a nested view lose to the wrapper that contains it.
    u32 order = 0;
};

/// A view whose subtree is malformed, reported rather than asserted.
struct sv::plan_diagnostic
{
    diagnostic_reason reason = diagnostic_reason::cycle;
    view_id id;                         ///< the view the descent stopped at
    layout_node_id node = invalid_node; ///< the leaf that referenced it
};

/// What the renderer already knows about a view from previous frames — the only input the builder cannot get from a definition.
///
/// Passed in rather than reached out of `view_renderer`, which is what keeps `build_render_plan` a pure function that
/// a test can drive for two hundred frames without a GPU.
struct sv::view_history_entry
{
    bool exists = false; ///< whether a usable target is already held
    tg::vec2i resolution = tg::vec2i(0, 0);
    u64 last_refresh_frame = 0;
};

struct sv::view_history
{
    cc::map<view_id, view_history_entry> entries;

    [[nodiscard]] view_history_entry lookup(view_id id) const;
};

/// A frame's recording order, with the recursive tree already flattened.
///
/// Holds no GPU resources — only ids, sizes, formats, rects and uv rects — so building one is a pure function and every
/// invariant below is testable headless.
///
/// The invariant the executor leans on: **for every draw, its source target index is strictly below the index of the
/// target that draw writes.** A bottom-up post-order append is what establishes it, and `validate` re-checks it — a
/// violation is a sampled-while-being-written bug rather than a visual glitch.
struct sv::render_plan
{
    /// In dependency order; the frame's output is always last.
    cc::vector<plan_target> targets;
    cc::vector<plan_trace> traces;

    /// Every target's draws, concatenated; target `i` owns `[target_first_draw[i], + target_draw_count[i])`.
    cc::vector<layout_draw> draws;
    cc::vector<u32> target_first_draw;
    cc::vector<u32> target_draw_count;

    cc::vector<hit_region> hit_regions;
    cc::vector<plan_diagnostic> diagnostics;

    /// Every view the frame reaches, refreshing or not.
    ///
    /// A throttled view records nothing, but its texture must still be touched or the cache's idle reclaim releases it
    /// out from under the parent that samples it.
    /// This is the list the renderer touches.
    cc::vector<view_id> reachable;

    [[nodiscard]] cc::span<layout_draw const> draws_of(u32 target) const;

    /// Re-checks the ordering invariant and the per-target draw ranges.
    [[nodiscard]] bool validate() const;
};

namespace sv
{
/// The region a window-space point lands in, or `invalid_hit_region` when it lands in none.
///
/// Picks in true painter's order rather than by depth or by a counter: a region nested inside another is in front of
/// it, and between two branches the one whose enclosing draw came later wins.
[[nodiscard]] u32 pick_hit_region(cc::span<hit_region const> regions, tg::pos2f window_point);

/// Flattens `def` into the order a frame records it, with `def.root_view` filling `output_size`.
///
/// `frame_index` is the loop's monotonic counter and drives the refresh policies; it is an argument rather than a wall
/// clock so a test is deterministic.
[[nodiscard]] render_plan build_render_plan(viewer_definition const& def,
                                            tg::vec2i output_size,
                                            u64 frame_index,
                                            view_history const& history);
} // namespace sv
