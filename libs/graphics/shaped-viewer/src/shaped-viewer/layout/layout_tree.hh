#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/box_style.hh>
#include <shaped-viewer/layout/solvers.hh>
#include <shaped-viewer/view/post_process.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// How a layout node arranges its children.
///
/// `grid` places every flowed child in a cell, its `grid_params` covering linear runs and automatic grids alike;
/// `relative` places its single child by fraction and is itself out of its parent's flow;
/// `leaf` is where views actually land.
enum class sv::layout_kind : sv::u8
{
    grid,
    relative,
    leaf,
};

/// How a leaf reconciles the resolution its views rendered at with the rect it was given.
enum class sv::fit_mode : sv::u8
{
    /// The whole source fills the whole rect, aspect not preserved.
    stretch,

    /// One source texel per target pixel, centered — a larger source is cropped, a smaller one insets the rect.
    native,
};
// todo: fill (cover the rect keeping aspect), contain (letterbox keeping aspect), crop (1:1 anchored, not centered)

/// How a leaf's views are sampled when the rect and the source resolution disagree.
enum class sv::sampler_mode : sv::u8
{
    nearest,
    linear,
};

namespace sv
{
// todo: cubic, plus a mip-aware minification filter once view targets carry mips
} // namespace sv

/// Where a `relative` node sits inside its parent's content box.
///
/// The fractions are of the parent's content box, measured from its top-left; the pixel offsets are added afterwards,
/// so a fixed-size inset in a corner is `size = (0, 0)` plus a `size_offset`, and a proportional one needs no offsets.
struct sv::relative_placement
{
    tg::pos2f position = tg::pos2f(0, 0);
    tg::vec2f size = tg::vec2f(1, 1);
    tg::vec2i position_offset = tg::vec2i(0, 0);
    tg::vec2i size_offset = tg::vec2i(0, 0);
};

/// A layout leaf: which views land here, how they are combined, and how the result meets the leaf's rect.
///
/// The default is the single view every caller writes; more than one only makes sense under a post-process that
/// combines them, such as a wipe between two takes on the same scene.
struct sv::layout_leaf
{
    /// Slots in the frame's view pool.
    cc::vector<view_index> views;

    /// Combines `views` into one image.
    /// Empty with one view is a plain blit; the plan builder reports a count that disagrees with the kind.
    cc::vector<post_process> post_processes;

    fit_mode fit = fit_mode::stretch;
    sampler_mode sampler = sampler_mode::linear;

    /// Whether the key-bound zoom may magnify this leaf.
    /// The zoom lives in persistent state and resolves into what the leaf samples, never into a camera.
    bool allow_zoom = true;

    /// Magnification into what this leaf samples: 1 shows the whole source, 4 shows a quarter of it per axis.
    ///
    /// Resolved from persistent state before the plan is built, so it arrives here as plain data — which is what keeps
    /// it out of every trace hash, and so keeps zooming from restarting a converged image.
    float zoom = 1.0f;

    /// Where the magnified window sits, in the source's own uv.
    /// Clamped so the window never leaves the source, which is why zooming out always lands back on the whole image.
    tg::pos2f zoom_center = tg::pos2f(0.5f, 0.5f);
};

/// One node of a layout tree.
///
/// Children are held by index into the owning pool, so a tree is a flat value that copies without pointer fixups.
/// Only the fields this node's `kind` reads apply — `grid` for a grid, `placement` for `relative`, `leaf` for a leaf —
/// and `style` applies to every kind.
struct sv::layout_node
{
    layout_kind kind = layout_kind::grid;
    box_style style = {};
    grid_params grid = {};
    relative_placement placement = {};
    cc::vector<layout_node_id> children;
    layout_leaf leaf = {};
};

/// The flat pool of layout nodes a frame builds.
///
/// One pool holds every layout in a frame: a nested layout layer is just another root inside it, which is what lets a
/// view's sub-tree be built with the same calls as the top-level one and referenced by a single index.
struct sv::layout_tree
{
    cc::vector<layout_node> nodes;

    /// Appends a grid container under `parent`, or as a fresh root when `parent == invalid_node`.
    layout_node_id add_container(layout_node_id parent, box_style style = {}, grid_params grid = {});

    /// Appends a leaf under `parent`.
    layout_node_id add_leaf(layout_node_id parent, layout_leaf leaf, box_style style = {});

    /// Appends a relative container under `parent`.
    /// It holds one child and is out of `parent`'s flow: siblings tile as if it were absent, and it draws in front.
    layout_node_id add_relative(layout_node_id parent, relative_placement placement, box_style style = {});

    [[nodiscard]] layout_node& operator[](layout_node_id i) { return nodes[u32(i)]; }
    [[nodiscard]] layout_node const& operator[](layout_node_id i) const { return nodes[u32(i)]; }
    [[nodiscard]] bool empty() const { return nodes.empty(); }
    void clear() { nodes.clear(); }
};

/// One thing a resolved layout puts on its target.
struct sv::resolved_item
{
    enum class item_kind : u8
    {
        background, ///< a node's whole border box, filled flat — what gives its padding and gutters a color
        border,     ///< one band of a node's border ring
        leaf,       ///< a leaf's content box, to be filled by whatever its views render
    };

    item_kind kind = item_kind::leaf;
    layout_node_id node = invalid_node;
    tg::aabb2i rect = {};
    tg::vec4f color = tg::vec4f(0, 0, 0, 0); ///< background and border only
};

/// A layout tree resolved against a rect: everything to draw, in the order to draw it.
///
/// One ordered list rather than a list per kind, because the order *is* the result: a node's border draws before its
/// children, and an out-of-flow `relative` subtree draws after every flowed sibling.
/// A caller replays it back to front.
struct sv::layout_solution
{
    cc::vector<resolved_item> items;
};

namespace sv
{
/// Walks the tree rooted at `root`, assigning each node a rect within `rect`, and returns everything to draw.
/// An empty tree, or an out-of-range root, resolves to nothing.
[[nodiscard]] layout_solution resolve_layout(layout_tree const& tree, layout_node_id root, tg::aabb2i rect);
} // namespace sv
