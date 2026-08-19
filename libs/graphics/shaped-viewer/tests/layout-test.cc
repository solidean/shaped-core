#include <clean-core/common/utility.hh> // cc::max, cc::min
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/layout/box_style.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/layout/solvers.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

// CPU-only invariants of the viewer's layout — the box model, the auto-grid choice, rect subdivision and the walk.
// No GPU, so these run in every configuration.

using namespace cc::primitive_defines;

namespace
{
// A 16:9 landscape window and a 9:16 portrait one — the aspect drives the auto-grid choice.
constexpr float landscape_16_9 = 16.0f / 9.0f;
constexpr float portrait_9_16 = 9.0f / 16.0f;

[[nodiscard]] int rect_w(tg::aabb2i const& r)
{
    return r.max[0] - r.min[0];
}
[[nodiscard]] int rect_h(tg::aabb2i const& r)
{
    return r.max[1] - r.min[1];
}
[[nodiscard]] tg::aabb2i rect_of(int x0, int y0, int x1, int y1)
{
    return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x1, y1));
}

/// The leaf items of a solution, in order — what a caller actually fills.
[[nodiscard]] cc::vector<sv::resolved_item> leaves_of(sv::layout_solution const& s)
{
    auto out = cc::vector<sv::resolved_item>();
    for (auto const& i : s.items)
        if (i.kind == sv::resolved_item::item_kind::leaf)
            out.push_back(i);
    return out;
}

/// Appends a leaf naming exactly one view — the shape almost every layout is built from.
[[nodiscard]] sv::layout_node_id add_view_leaf(sv::layout_tree& t,
                                               sv::layout_node_id parent,
                                               u32 view,
                                               sv::box_style style = {})
{
    auto leaf = sv::layout_leaf{};
    leaf.views.push_back(sv::view_index(view));
    return t.add_leaf(parent, cc::move(leaf), style);
}
} // namespace

TEST("sv - auto_grid_dims on a landscape window")
{
    auto const p = sv::grid_params{};

    // The behavior the API promises: 1 fills, 2 side-by-side, 3 in a row, 4 as a 2x2, 5 as 3x2.
    auto const check = [&](int n, int cols, int rows)
    {
        auto const d = sv::auto_grid_dims(n, landscape_16_9, p);
        CHECK(d.cols == cols);
        CHECK(d.rows == rows);
        CHECK(d.cols * d.rows >= n); // always enough cells
    };

    check(1, 1, 1);
    check(2, 2, 1);
    check(3, 3, 1);
    check(4, 2, 2);
    check(5, 3, 2);
    check(6, 3, 2);
    check(9, 3, 3);
}

TEST("sv - auto_grid_dims flips on a portrait window")
{
    // Two views on a tall window stack vertically rather than sitting side-by-side.
    auto const d = sv::auto_grid_dims(2, portrait_9_16, {});
    CHECK(d.cols == 1);
    CHECK(d.rows == 2);
}

TEST("sv - a pinned dimension derives the other")
{
    // Row-wise fill: three columns take up to three views per row, and the row count follows the view count.
    CHECK(sv::resolve_grid_dims(5, landscape_16_9, {.cols = 3}).cols == 3);
    CHECK(sv::resolve_grid_dims(5, landscape_16_9, {.cols = 3}).rows == 2);
    CHECK(sv::resolve_grid_dims(6, landscape_16_9, {.cols = 3}).rows == 2);
    CHECK(sv::resolve_grid_dims(7, landscape_16_9, {.cols = 3}).rows == 3);

    // The degenerate pins are the two linear layouts, whichever the window's aspect would have preferred.
    auto const vertical = sv::resolve_grid_dims(4, landscape_16_9, {.cols = 1});
    CHECK(vertical.cols == 1);
    CHECK(vertical.rows == 4);
    auto const horizontal = sv::resolve_grid_dims(4, portrait_9_16, {.rows = 1});
    CHECK(horizontal.cols == 4);
    CHECK(horizontal.rows == 1);

    // Both pinned is the fixed grid: it neither grows for extra views nor shrinks for missing ones.
    auto const fixed = sv::resolve_grid_dims(7, landscape_16_9, {.cols = 2, .rows = 2});
    CHECK(fixed.cols == 2);
    CHECK(fixed.rows == 2);

    // Neither pinned is the auto-grid.
    CHECK(sv::resolve_grid_dims(4, landscape_16_9, {}).cols == sv::auto_grid_dims(4, landscape_16_9, {}).cols);
}

TEST("sv - subdivide_grid tiles a rect exactly")
{
    auto const cells = sv::subdivide_grid(rect_of(0, 0, 100, 100), 2, 2, /*spacing*/ 0);

    REQUIRE(cells.size() == 4);

    // Row-major: cell 0 top-left, cell 3 bottom-right.
    CHECK(cells[0] == rect_of(0, 0, 50, 50));
    CHECK(cells[3] == rect_of(50, 50, 100, 100));

    // Cover exactly: the four areas sum to the whole rect, and columns/rows tile with no gap.
    auto area = 0;
    for (auto const& c : cells)
        area += rect_w(c) * rect_h(c);
    CHECK(area == 100 * 100);
    CHECK(cells[0].max[0] == cells[1].min[0]); // adjacent columns meet
}

TEST("sv - subdivide_grid pins the far edge under rounding")
{
    // An odd width must still land its last edge exactly on the rect's far edge (no lost/overshot pixel).
    auto const cells = sv::subdivide_grid(rect_of(0, 0, 101, 100), 2, 1, /*spacing*/ 0);

    REQUIRE(cells.size() == 2);
    CHECK(cells[0].min[0] == 0);
    CHECK(cells[1].max[0] == 101);             // far edge pinned
    CHECK(cells[0].max[0] == cells[1].min[0]); // no gap between them
}

TEST("sv - subdivide_grid honors spacing")
{
    // Spacing is now the solver's only inset: padding was already removed by the box model before we get here.
    auto const cells = sv::subdivide_grid(rect_of(0, 0, 100, 100), 2, 1, /*spacing*/ 10);

    REQUIRE(cells.size() == 2);
    CHECK(cells[0].min == tg::pos2i(0, 0));
    CHECK(cells[1].max == tg::pos2i(100, 100));
    CHECK(cells[1].min[0] - cells[0].max[0] == 10);
}

TEST("sv - the box model insets outside-in")
{
    auto const style = sv::box_style{.margin = 2, .border = 3, .padding = 4};
    auto const rect = rect_of(0, 0, 100, 100);

    // Margin first, then the border band, then padding — so the content box is inset by the sum.
    CHECK(sv::border_box(rect, style) == rect_of(2, 2, 98, 98));
    CHECK(sv::content_box(rect, style) == rect_of(9, 9, 91, 91));
}

TEST("sv - per-side insets apply per side")
{
    auto const insets = sv::box_insets(1, 2, 3, 4); // left, top, right, bottom
    CHECK(sv::inset(rect_of(0, 0, 100, 100), insets) == rect_of(1, 2, 97, 96));

    // The shorter spellings are the same type: one value is all four sides, two are horizontal and vertical.
    CHECK(sv::box_insets(5) == sv::box_insets(5, 5, 5, 5));
    CHECK(sv::box_insets(5, 9) == sv::box_insets(5, 9, 5, 9));
    CHECK(sv::box_insets::all(5) == sv::box_insets(5));
    CHECK(sv::box_insets::symmetric(5, 9) == sv::box_insets(5, 9));
}

TEST("sv - an inset collapses rather than inverting")
{
    // A rect too small for its insets must stay empty, so every downstream extent stays >= 0.
    auto const r = sv::inset(rect_of(0, 0, 10, 10), sv::box_insets::all(20));
    CHECK(rect_w(r) == 0);
    CHECK(rect_h(r) == 0);
    CHECK(r.min[0] <= r.max[0]);
    CHECK(r.min[1] <= r.max[1]);
}

TEST("sv - border_bands cover the ring exactly once")
{
    tg::aabb2i bands[4] = {};
    auto const rect = rect_of(0, 0, 100, 50);
    auto const count = sv::border_bands(rect, 5, bands);

    REQUIRE(count == 4);

    // The ring is the rect minus its inner box, and the bands must tile it without overlapping — an overlap would
    // double-blend a corner once borders are drawn with alpha.
    auto area = 0;
    for (auto i = 0; i < count; ++i)
        area += rect_w(bands[i]) * rect_h(bands[i]);
    CHECK(area == 100 * 50 - 90 * 40);

    for (auto i = 0; i < count; ++i)
        for (auto j = i + 1; j < count; ++j)
        {
            auto const overlap_w = cc::min(bands[i].max[0], bands[j].max[0]) - cc::max(bands[i].min[0], bands[j].min[0]);
            auto const overlap_h = cc::min(bands[i].max[1], bands[j].max[1]) - cc::max(bands[i].min[1], bands[j].min[1]);
            CHECK((overlap_w <= 0 || overlap_h <= 0));
        }
}

TEST("sv - an over-thick border swallows the rect as one band")
{
    tg::aabb2i bands[4] = {};
    auto const rect = rect_of(0, 0, 10, 10);
    auto const count = sv::border_bands(rect, 20, bands);

    REQUIRE(count == 1);
    CHECK(bands[0] == rect);
}

TEST("sv - resolve_layout nests a vertical inside the auto-grid root")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node);
    auto const column = tree.add_container(root, {}, {.cols = 1});
    add_view_leaf(tree, column, 10);
    add_view_leaf(tree, column, 11);

    auto const leaves = leaves_of(sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100)));

    REQUIRE(leaves.size() == 2);

    // The single container fills the auto-grid root, then splits top/bottom for its two views.
    CHECK(tree[leaves[0].node].leaf.views[0] == sv::view_index(10));
    CHECK(leaves[0].rect == rect_of(0, 0, 100, 50));
    CHECK(tree[leaves[1].node].leaf.views[0] == sv::view_index(11));
    CHECK(leaves[1].rect == rect_of(0, 50, 100, 100));
}

TEST("sv - resolve_layout auto-grids the root's leaves")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node);
    add_view_leaf(tree, root, 0);
    add_view_leaf(tree, root, 1);

    auto const leaves = leaves_of(sv::resolve_layout(tree, root, rect_of(0, 0, 160, 90)));

    // Two views on a 16:9 root sit side-by-side (2x1).
    REQUIRE(leaves.size() == 2);
    CHECK(leaves[0].rect == rect_of(0, 0, 80, 90));
    CHECK(leaves[1].rect == rect_of(80, 0, 160, 90));
}

TEST("sv - a container's padding and spacing shrink its cells")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node, {.padding = 5, .spacing = 10}, {.rows = 1});
    add_view_leaf(tree, root, 0);
    add_view_leaf(tree, root, 1);

    auto const leaves = leaves_of(sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100)));

    REQUIRE(leaves.size() == 2);

    // Padding insets the content box on every side before the split; spacing then separates the two cells inside it.
    CHECK(leaves[0].rect == rect_of(5, 5, 45, 95));
    CHECK(leaves[1].rect == rect_of(55, 5, 95, 95));
    CHECK(leaves[1].rect.min[0] - leaves[0].rect.max[0] == 10);
}

TEST("sv - a relative node leaves the flow and draws in front")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node);

    // A quarter-size inset anchored a tenth in from the top-left.
    auto const overlay = tree.add_relative(root, {.position = tg::pos2f(0.1f, 0.1f), .size = tg::vec2f(0.25f, 0.25f)});
    add_view_leaf(tree, overlay, 0);
    add_view_leaf(tree, root, 1);
    add_view_leaf(tree, root, 2);

    auto const leaves = leaves_of(sv::resolve_layout(tree, root, rect_of(0, 0, 160, 90)));
    REQUIRE(leaves.size() == 3);

    // The two flowed leaves tile as if the relative one were absent: a 2x1 split, not two thirds of a 3-cell grid.
    CHECK(tree[leaves[0].node].leaf.views[0] == sv::view_index(1));
    CHECK(leaves[0].rect == rect_of(0, 0, 80, 90));
    CHECK(tree[leaves[1].node].leaf.views[0] == sv::view_index(2));
    CHECK(leaves[1].rect == rect_of(80, 0, 160, 90));

    // The relative one emits last, so it lands over the siblings it overlaps, at its own fraction of the root.
    // A half-pixel fraction rounds away from zero: 0.25 * 90 is 22.5, so the height is 23.
    CHECK(tree[leaves[2].node].leaf.views[0] == sv::view_index(0));
    CHECK(leaves[2].rect == rect_of(16, 9, 56, 32));
}

TEST("sv - a relative node's pixel offsets place it absolutely")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node);

    // Zero fractions with offsets is the "keep exactly this rect" form a dragged view uses.
    auto const placed = tree.add_relative(root, {.position = tg::pos2f(0, 0),
                                                 .size = tg::vec2f(0, 0),
                                                 .position_offset = tg::vec2i(10, 20),
                                                 .size_offset = tg::vec2i(40, 40)});
    add_view_leaf(tree, placed, 7);

    auto const leaves = leaves_of(sv::resolve_layout(tree, root, rect_of(0, 0, 160, 90)));
    REQUIRE(leaves.size() == 1);
    CHECK(leaves[0].rect == rect_of(10, 20, 50, 60));
}

TEST("sv - an all-relative container still emits every leaf")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node, {}, {.cols = 1});

    auto const a = tree.add_relative(root, {.position = tg::pos2f(0, 0), .size = tg::vec2f(0.1f, 0.1f)});
    add_view_leaf(tree, a, 7);
    auto const b = tree.add_relative(root, {.position = tg::pos2f(0.2f, 0.2f), .size = tg::vec2f(0.1f, 0.1f)});
    add_view_leaf(tree, b, 8);

    // Nothing is left to subdivide, and the container must not swallow its children on the way out.
    auto const leaves = leaves_of(sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100)));
    REQUIRE(leaves.size() == 2);
    CHECK(leaves[0].rect == rect_of(0, 0, 10, 10));
    CHECK(leaves[1].rect == rect_of(20, 20, 30, 30));
}

TEST("sv - a border emits before the content it frames")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node, {.border = 2, .border_color = tg::vec4f(1, 0, 0, 1)});
    add_view_leaf(tree, root, 0);

    auto const solution = sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100));

    // Order is the result: every band of the frame lands before the leaf that sits inside it.
    REQUIRE(solution.items.size() == 5);
    for (auto i = 0; i < 4; ++i)
        CHECK(solution.items[i].kind == sv::resolved_item::item_kind::border);
    CHECK(solution.items[4].kind == sv::resolved_item::item_kind::leaf);
    CHECK(solution.items[4].rect == rect_of(2, 2, 98, 98));
}

TEST("sv - a background fills the border box, under the border and through the padding")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(sv::invalid_node, {.margin = 4,
                                                            .border = 2,
                                                            .border_color = tg::vec4f(1, 0, 0, 1),
                                                            .background_color = tg::vec4f(0, 0, 1, 1),
                                                            .padding = 8});
    add_view_leaf(tree, root, 0);

    auto const solution = sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100));

    // The fill goes down first, so the border and every child cover it.
    REQUIRE(solution.items.size() == 6);
    CHECK(solution.items[0].kind == sv::resolved_item::item_kind::background);
    CHECK(solution.items[0].color == tg::vec4f(0, 0, 1, 1));
    for (auto i = 1; i < 5; ++i)
        CHECK(solution.items[i].kind == sv::resolved_item::item_kind::border);

    // The margin is outside the fill and the padding is inside it: that gap is exactly what a background colors.
    CHECK(solution.items[0].rect == rect_of(4, 4, 96, 96));
    CHECK(solution.items[5].rect == rect_of(14, 14, 86, 86));
}

TEST("sv - a background is emitted only when it would be visible")
{
    auto tree = sv::layout_tree{};

    // Unset is fully transparent, and so is an explicit color with no alpha — neither is worth a draw.
    CHECK(!sv::has_visible_background({}));
    CHECK(!sv::has_visible_background({.background_color = tg::vec4f(1, 1, 1, 0)}));
    CHECK(sv::has_visible_background({.background_color = tg::vec4f(0, 0, 0, 0.5f)}));

    auto const root = tree.add_container(sv::invalid_node, {.background_color = tg::vec4f(1, 1, 1, 0)});
    add_view_leaf(tree, root, 0);

    auto const solution = sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100));
    REQUIRE(solution.items.size() == 1);
    CHECK(solution.items[0].kind == sv::resolved_item::item_kind::leaf);
}

TEST("sv - a container's background covers the gutters between its children")
{
    auto tree = sv::layout_tree{};
    auto const root = tree.add_container(
        sv::invalid_node, {.background_color = tg::vec4f(0.2f, 0.2f, 0.2f, 1), .spacing = 10}, {.rows = 1});
    add_view_leaf(tree, root, 0);
    add_view_leaf(tree, root, 1);

    auto const solution = sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100));
    auto const leaves = leaves_of(solution);

    // Spacing lives in the parent's content box, so the parent's fill is what shows through between the two cells.
    REQUIRE(leaves.size() == 2);
    CHECK(solution.items[0].kind == sv::resolved_item::item_kind::background);
    CHECK(solution.items[0].rect == rect_of(0, 0, 100, 100));
    CHECK(leaves[0].rect == rect_of(0, 0, 45, 100));
    CHECK(leaves[1].rect == rect_of(55, 0, 100, 100));
}

TEST("sv - an invisible border still reserves its space")
{
    auto tree = sv::layout_tree{};
    // A width with no color is the gutter form: nothing is drawn, but the content still moves in.
    auto const root = tree.add_container(sv::invalid_node, {.border = 5});
    add_view_leaf(tree, root, 0);

    auto const solution = sv::resolve_layout(tree, root, rect_of(0, 0, 100, 100));
    REQUIRE(solution.items.size() == 1);
    CHECK(solution.items[0].kind == sv::resolved_item::item_kind::leaf);
    CHECK(solution.items[0].rect == rect_of(5, 5, 95, 95));
}

TEST("sv - an empty tree resolves to nothing")
{
    auto const tree = sv::layout_tree{};
    CHECK(sv::resolve_layout(tree, sv::layout_node_id(0), rect_of(0, 0, 100, 100)).items.empty());
    CHECK(sv::resolve_layout(tree, sv::invalid_node, rect_of(0, 0, 100, 100)).items.empty());
}
