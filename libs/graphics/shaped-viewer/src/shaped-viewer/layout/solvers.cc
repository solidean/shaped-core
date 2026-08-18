#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::max
#include <shaped-viewer/layout/solvers.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs, tg::log, tg::round

#include <limits>

namespace sv
{
namespace
{
[[nodiscard]] int ceil_div(int a, int b)
{
    return (a + b - 1) / b;
}

[[nodiscard]] int iround(double x)
{
    return int(tg::round(x));
}
} // namespace

grid_dims resolve_grid_dims(int cell_count, float area_aspect, grid_params const& p)
{
    auto const n = cc::max(cell_count, 1);
    auto const cols = p.cols.has_value() ? cc::max(p.cols.value(), 1) : 0;
    auto const rows = p.rows.has_value() ? cc::max(p.rows.value(), 1) : 0;

    if (cols > 0 && rows > 0)
        return {.cols = cols, .rows = rows};
    if (cols > 0)
        return {.cols = cols, .rows = ceil_div(n, cols)};
    if (rows > 0)
        return {.cols = ceil_div(n, rows), .rows = rows};

    return auto_grid_dims(cell_count, area_aspect, p);
}

grid_dims auto_grid_dims(int cell_count, float area_aspect, grid_params const& p)
{
    if (cell_count <= 1)
        return {.cols = 1, .rows = 1};

    auto const target = p.target_aspect > 0.0f ? p.target_aspect : 1.0f;

    auto best = grid_dims{.cols = 1, .rows = cell_count};
    auto best_score = std::numeric_limits<float>::infinity();

    for (auto cols = 1; cols <= cell_count; ++cols)
    {
        auto const rows = ceil_div(cell_count, cols);
        auto const cell_aspect = area_aspect * float(rows) / float(cols);
        auto const aspect_term = tg::abs(tg::log(cell_aspect / target));
        auto const waste = float(cols * rows - cell_count);
        auto const score = aspect_term + p.empty_penalty * waste;

        // Track the true minimum; on a near-tie prefer more columns (fewer rows) — wider suits landscape.
        if (score <= best_score + 1e-4f)
        {
            if (score < best_score)
                best_score = score;
            best = {.cols = cols, .rows = rows};
        }
    }

    return best;
}

cc::vector<tg::aabb2i> subdivide_grid(tg::aabb2i rect, int cols, int rows, int spacing)
{
    CC_ASSERT(cols >= 1 && rows >= 1, "grid needs at least one column and row");

    auto const x0 = rect.min[0];
    auto const y0 = rect.min[1];
    auto const inner_w = rect.max[0] - x0 > 0 ? rect.max[0] - x0 : 0;
    auto const inner_h = rect.max[1] - y0 > 0 ? rect.max[1] - y0 : 0;

    // Distribute the space that is not spacing; integer content edges tile it exactly (edge(count) == content).
    auto const content_w = inner_w - spacing * (cols - 1);
    auto const content_h = inner_h - spacing * (rows - 1);
    auto const cx = [&](int k) { return x0 + iround(double(k) * double(content_w) / cols) + spacing * k; };
    auto const cy = [&](int k) { return y0 + iround(double(k) * double(content_h) / rows) + spacing * k; };

    auto cells = cc::vector<tg::aabb2i>();
    cells.reserve(isize(cols) * isize(rows));
    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < cols; ++c)
            cells.push_back(tg::aabb2i(tg::pos2i(cx(c), cy(r)), tg::pos2i(cx(c + 1) - spacing, cy(r + 1) - spacing)));
    return cells;
}

cc::vector<tg::aabb2i> subdivide_linear(tg::aabb2i rect, int n, bool horizontal, int spacing)
{
    return horizontal ? subdivide_grid(rect, n, 1, spacing) : subdivide_grid(rect, 1, n, spacing);
}
} // namespace sv
