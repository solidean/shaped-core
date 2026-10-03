#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/drawing/font.hh>
#include <shaped-viewer/drawing/instance.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// Annotations: a label flat on screen, anchored at a point of the scene, with a marker on the point and a leader to
// the label.
// libs/graphics/shaped-viewer/docs/canvas.md is the design.

/// Where an annotation's box sits relative to its anchor.
enum class sv::annotation_side : sv::u8
{
    /// Away from the view's centre, so labels fan outward; recomputed every frame as the anchor moves.
    automatic,
    above_right,
    above_left,
    below_right,
    below_left,
};

/// What an annotation draws while its anchor is hidden behind the scene's geometry.
enum class sv::annotation_occluded : sv::u8
{
    /// The box stays, the marker turns hollow and the leader dashed: still readable, visibly behind.
    hidden_line,

    /// Nothing at all.
    hide,

    /// Everything, as if the anchor were in view: for a point inside a part, such as its centre of mass.
    show,
};

enum class sv::leader_shape : sv::u8
{
    /// One straight line from the marker to the nearest point of the box.
    straight,

    /// A line to a short horizontal shelf that runs into the side of the box facing the anchor.
    elbow,
};

/// The line from an annotation's marker to its box, in logical pixels.
struct sv::leader_style
{
    leader_shape shape = leader_shape::elbow;
    f32 width = 1.0f;

    /// Straight alpha, sRGB-encoded, in [0, 1]; the marker takes it too.
    tg::vec4f color = tg::vec4f(1, 1, 1, 0.9f);

    /// Alternating on and off lengths for the leader while the anchor is visible; empty is solid.
    cc::vector<f32> dashes;

    /// The same while the anchor is hidden, under `annotation_occluded::hidden_line`.
    cc::vector<f32> hidden_dashes = {4, 3};
};

/// How an annotation looks and where its box goes, in logical pixels.
struct sv::annotation_style
{
    text_style text = {.size = 13.0f};

    /// Straight alpha, sRGB-encoded, in [0, 1]; a border of width 0 draws none.
    tg::vec4f fill = tg::vec4f(0.08f, 0.1f, 0.14f, 0.85f);
    tg::vec4f border = tg::vec4f(1, 1, 1, 0.35f);
    f32 border_width = 1.0f;
    f32 corner_radius = 4.0f;

    /// Between the text and the box's edge, horizontally and vertically.
    tg::vec2f padding = tg::vec2f(7, 4);

    annotation_side side = annotation_side::automatic;

    /// From the anchor to the box's corner nearest it, along each axis away from the anchor.
    tg::vec2f offset = tg::vec2f(32, 28);

    /// Kept between the box and the view's edges; a box that would cross one is pushed back in.
    f32 margin = 8.0f;

    leader_style leader;
    f32 marker_radius = 3.5f;

    annotation_occluded occluded = annotation_occluded::hidden_line;
};

/// One annotation as a layer keeps it: its anchor, and everything it draws resolved to atlas records, in logical pixels.
/// Its place on screen is not here: the plan computes it every frame from the camera, which this does not know.
struct sv::annotation_record
{
    tg::pos3f anchor;

    /// The box and the text in it, `at` measured from the box's top-left corner.
    cc::vector<drawing_placement> content;
    tg::vec2f box_size = tg::vec2f(0, 0);

    annotation_side side = annotation_side::automatic;
    tg::vec2f offset = tg::vec2f(32, 28);
    f32 margin = 8.0f;
    annotation_occluded occluded = annotation_occluded::hidden_line;

    leader_shape leader = leader_shape::elbow;
    f32 leader_width = 1.0f;
    u32 leader_color = 0xffffffff;
    cc::vector<f32> dashes;
    cc::vector<f32> hidden_dashes;

    /// The marker's parts, each drawn in logical pixels around the anchor: a filled disk and a ring of the same radius.
    f32 marker_radius = 3.5f;
    drawing_placement disk;
    drawing_placement ring;

    /// A unit segment from (0, -0.5) to (1, 0.5), and a disk of radius 1, from which leaders are built.
    drawing_placement segment;
    drawing_placement dot;
};

namespace sv::impl
{
/// Appends what `a` draws this frame to `out`, in logical pixels of a view `logical_size` across.
///
/// `world_to_clip` is the camera the anchor is projected through; an anchor behind it or outside the view draws
/// nothing.
/// Every placement carries a probe of the anchor, so whether the anchor is hidden is decided on the GPU against the
/// trace's depth, never here.
void place_annotation(annotation_record const& a,
                      tg::mat4f const& world_to_clip,
                      tg::vec2f logical_size,
                      cc::vector<drawing_placement>& out);
} // namespace sv::impl
