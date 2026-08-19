#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::max, cc::min, cc::move
#include <shaped-viewer/layout/layout_tree.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::round

namespace sv
{
namespace
{
[[nodiscard]] int iround(double x)
{
    return int(tg::round(x));
}

/// width / height of a pixel rect, guarding a zero (or inverted) height.
[[nodiscard]] float rect_aspect(tg::aabb2i const& r)
{
    auto const w = r.max[0] - r.min[0];
    auto const h = r.max[1] - r.min[1];
    return h > 0 ? float(w) / float(h) : 1.0f;
}

/// Where a `relative` child sits inside `content`, its parent's content box.
[[nodiscard]] tg::aabb2i place_relative(tg::aabb2i content, relative_placement const& p)
{
    auto const w = content.max[0] - content.min[0];
    auto const h = content.max[1] - content.min[1];

    auto const x0 = content.min[0] + iround(double(p.position[0]) * double(w)) + p.position_offset[0];
    auto const y0 = content.min[1] + iround(double(p.position[1]) * double(h)) + p.position_offset[1];
    auto const x1 = x0 + iround(double(p.size[0]) * double(w)) + p.size_offset[0];
    auto const y1 = y0 + iround(double(p.size[1]) * double(h)) + p.size_offset[1];

    return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(cc::max(x1, x0), cc::max(y1, y0)));
}

void resolve_node(layout_tree const& tree, layout_node_id index, tg::aabb2i rect, layout_solution& out);

/// Resolves `child` against the cell it was given, unless it is `relative` — which ignores the cell and places itself
/// inside `parent_content` instead.
/// One helper so every parent kind, including `relative` itself, agrees on that rule.
void resolve_child(layout_tree const& tree,
                   layout_node_id child,
                   tg::aabb2i cell,
                   tg::aabb2i parent_content,
                   layout_solution& out)
{
    auto const& node = tree[child];
    resolve_node(tree, child,
                 node.kind == layout_kind::relative ? place_relative(parent_content, node.placement) : cell, out);
}

void resolve_node(layout_tree const& tree, layout_node_id index, tg::aabb2i rect, layout_solution& out)
{
    auto const& node = tree[index];

    // The background fills the whole border box — under the border and through the padding — so it goes down first.
    // That is what colors the padding and the gutters between this node's children: neither has a color of its own.
    if (has_visible_background(node.style))
        out.items.push_back({.kind = resolved_item::item_kind::background,
                             .node = index,
                             .rect = border_box(rect, node.style),
                             .color = node.style.background_color});

    // The border draws before anything inside it, so a child that overflows its cell still covers the frame.
    if (has_visible_border(node.style))
    {
        tg::aabb2i bands[4] = {};
        auto const count = border_bands(border_box(rect, node.style), node.style.border, bands);
        for (auto i = 0; i < count; ++i)
            out.items.push_back({.kind = resolved_item::item_kind::border,
                                 .node = index,
                                 .rect = bands[i],
                                 .color = node.style.border_color});
    }

    auto const content = content_box(rect, node.style);

    if (node.kind == layout_kind::leaf)
    {
        out.items.push_back({.kind = resolved_item::item_kind::leaf, .node = index, .rect = content});
        return;
    }

    if (node.kind == layout_kind::relative)
    {
        CC_ASSERT(node.children.size() <= 1, "a relative layout node holds at most one child");
        if (!node.children.empty())
            resolve_child(tree, node.children[0], content, content, out);
        return;
    }

    // A relative child takes no cell, so its siblings tile as if it were absent.
    auto flowed = cc::vector<layout_node_id>();
    auto out_of_flow = cc::vector<layout_node_id>();
    flowed.reserve(node.children.size());
    for (auto const child : node.children)
        (tree[child].kind == layout_kind::relative ? out_of_flow : flowed).push_back(child);

    auto const n = int(flowed.size());
    if (n > 0)
    {
        auto const dims = resolve_grid_dims(n, rect_aspect(content), node.grid);
        auto const cells = subdivide_grid(content, dims.cols, dims.rows, node.style.spacing);

        // Row-major: child i lands in cell i; a grid with more cells than children leaves the tail empty.
        auto const assignable = cc::min(int(cells.size()), n);
        for (auto i = 0; i < assignable; ++i)
            resolve_child(tree, flowed[i], cells[i], content, out);
    }

    // Out-of-flow children emit last, so they draw over every flowed sibling of this container.
    // That ordering holds within one container; ranking one against another container's subtree wants a real z-order.
    for (auto const child : out_of_flow)
        resolve_child(tree, child, content, content, out);
}
} // namespace

layout_node_id layout_tree::add_container(layout_node_id parent, box_style style, grid_params grid)
{
    auto const index = layout_node_id(nodes.size());
    nodes.push_back({.kind = layout_kind::grid, .style = style, .grid = grid});
    if (parent != invalid_node)
        (*this)[parent].children.push_back(index);
    return index;
}

layout_node_id layout_tree::add_leaf(layout_node_id parent, layout_leaf leaf, box_style style)
{
    auto const index = layout_node_id(nodes.size());
    nodes.push_back({.kind = layout_kind::leaf, .style = style, .leaf = cc::move(leaf)});
    if (parent != invalid_node)
        (*this)[parent].children.push_back(index);
    return index;
}

layout_node_id layout_tree::add_relative(layout_node_id parent, relative_placement placement, box_style style)
{
    auto const index = layout_node_id(nodes.size());
    nodes.push_back({.kind = layout_kind::relative, .style = style, .placement = placement});
    if (parent != invalid_node)
        (*this)[parent].children.push_back(index);
    return index;
}

layout_solution resolve_layout(layout_tree const& tree, layout_node_id root, tg::aabb2i rect)
{
    auto out = layout_solution();
    if (root != invalid_node && isize(u32(root)) < tree.nodes.size())
        resolve_node(tree, root, rect, out);
    return out;
}
} // namespace sv
