#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

/// How a grid container is dimensioned — the one knob that spans a linear run, a fixed grid and an automatic one.
///
/// A set dimension is pinned (clamped to >= 1); an unset one is derived, so `cols = 1` is a vertical stack, `rows = 1`
/// a horizontal run, and setting neither leaves both to the auto-grid.
/// Children always fill row-major, so `cols = 3` puts up to three of them in each row.
struct sv::grid_params
{
    cc::optional<int> cols = {};
    cc::optional<int> rows = {};

    /// The cell aspect ratio (width / height) a derived dimension is scored against.
    /// ~1.0 reads well for a 3D view; larger favors wider cells.
    /// Only read when both dimensions are derived — a pinned one fixes the other by the child count alone.
    float target_aspect = 1.0f;

    /// Score added per empty cell (rows*cols - cell_count), under the same "both derived" condition.
    /// A real weight, not a tie-break — without it four views on a wide window pick 3x2 over the wanted 2x2.
    float empty_penalty = 0.25f;
};

/// The chosen column / row count of a grid.
struct sv::grid_dims
{
    int cols = 1;
    int rows = 1;
};

namespace sv
{
/// The dimensions a container tiles `cell_count` children into, applying `p`'s pinned / derived rule.
///
/// A pinned dimension fixes the other by ceil-division, which is what makes `cols = 1` a column of cells and
/// `rows = 1` a row of them; with neither pinned this is `auto_grid_dims`.
/// Both returned counts are >= 1, so a caller may subdivide with them unchecked.
[[nodiscard]] grid_dims resolve_grid_dims(int cell_count, float area_aspect, grid_params const& p);

/// Chooses (cols, rows) for `cell_count` cells filling an area of aspect `area_aspect` (width / height).
/// Each candidate is scored by how close its cell aspect lands to `p.target_aspect`, penalizing empty cells.
///
/// On a 16:9 area this yields 1->1x1, 2->2x1, 3->3x1, 4->2x2, 5->3x2.
/// `cell_count <= 1` is always 1x1.
[[nodiscard]] grid_dims auto_grid_dims(int cell_count, float area_aspect, grid_params const& p);

/// Splits `rect` into `cols` x `rows` cells in row-major order (row 0 left-to-right, then row 1, ...),
/// leaving `spacing` pixels between adjacent cells.
///
/// `rect` is already a content box — margin, border and padding were removed by the caller, so nothing here insets.
/// Integer edges tile the rect exactly, with no lost pixels and no overlap.
[[nodiscard]] cc::vector<tg::aabb2i> subdivide_grid(tg::aabb2i rect, int cols, int rows, int spacing);

/// A one-dimensional run of `n` cells across `rect` — a row when `horizontal`, else a column.
[[nodiscard]] cc::vector<tg::aabb2i> subdivide_linear(tg::aabb2i rect, int n, bool horizontal, int spacing);
} // namespace sv
