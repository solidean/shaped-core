#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/ray.hh>
#include <typed-geometry/geometry/query/hits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/spatial/fwd.hh>

/// A uniform grid: cell c spans origin + [c, c + 1) * cell_size along each axis, so a cell is named by a pos<D, i32>.
///
/// The grid holds nothing; it is the mapping between space and cells, for whoever keys their own data by cell.
/// Every component of `cell_size` must be > 0.
///
///     auto const grid = tg::uniform_grid3f{.origin = tg::pos3f::zero, .cell_size = tg::vec3f(16)};
///     for (auto const step : grid.cells_along(ray, 0.f, 100.f)) // front to back; break whenever
///         visit(step.cell, step.along);
template <int D, class T>
struct tg::uniform_grid
{
    using scalar_t = T;

    pos<D, T> origin;
    vec<D, T> cell_size;

    /// The cell holding `p`: floor per axis, so a point on a face between two cells is in the upper one.
    [[nodiscard]] constexpr pos<D, i32> cell_at(pos<D, T> const& p) const
    {
        auto c = pos<D, i32>();
        for (auto k = 0; k < D; ++k)
            c[k] = i32(tg::floor((p[k] - origin[k]) / cell_size[k]));
        return c;
    }

    /// Where cell `c`'s faces lie along `axis`: the lower at `c[axis]`, the upper at `c[axis] + 1`.
    [[nodiscard]] constexpr T plane_of(i32 index, int axis) const { return origin[axis] + T(index) * cell_size[axis]; }

    [[nodiscard]] constexpr aabb<D, T> box_of(pos<D, i32> const& c) const
    {
        auto lo = pos<D, T>();
        auto hi = pos<D, T>();
        for (auto k = 0; k < D; ++k)
        {
            lo[k] = plane_of(c[k], k);
            hi[k] = plane_of(c[k] + 1, k);
        }
        return aabb<D, T>(lo, hi);
    }

    /// The cells `r` passes through for its parameter in [t_min, t_max], front to back, each with the stretch inside it.
    /// Lazy, so a walk stops wherever its caller breaks; with an infinite t_max only the caller stops it.
    /// r.dir must not be zero.
    [[nodiscard]] constexpr grid_traversal<D, T> cells_along(ray<D, T> const& r, T t_min, T t_max) const
    {
        return grid_traversal<D, T>(*this, r, t_min, t_max);
    }
};

/// One cell a ray passes through: `along` is the ray's parameters inside it, from where it enters to where it leaves.
template <int D, class T>
struct tg::grid_step
{
    pos<D, i32> cell;
    hit_interval<T> along;
};

/// The cells a ray passes through, front to back: Amanatides and Woo's walk.
///
/// Every step's parameters are computed afresh from the integer cell, never summed up, so a long walk does not drift,
/// and where one cell's stretch ends the next one's begins, bit for bit.
/// Where the ray crosses several faces at once, through an edge or a corner, every axis crossed advances in one step,
/// so no cell is reported that the ray only touches.
/// A ray starting on a face starts in the cell its direction leads into, and an axis the ray runs parallel to is never crossed.
/// A walk of the one point t_min = t_max reports the cell holding it, and one with t_min > t_max reports nothing.
///
/// A stepper: next() hands out the steps one by one, and begin()/end() make it a range for range-for.
template <int D, class T>
class tg::grid_traversal
{
    // construction
public:
    constexpr grid_traversal(uniform_grid<D, T> const& grid, ray<D, T> const& r, T t_min, T t_max)
      : _grid(grid), _ray(r), _t(t_min), _t_max(t_max)
    {
        _done = !(t_min <= t_max);
        _is_point = t_min == t_max;
        if (_done)
            return;
        auto const start = r.at(t_min);
        _cell = grid.cell_at(start);
        for (auto k = 0; k < D; ++k)
        {
            _step[k] = r.dir[k] > T(0) ? 1 : r.dir[k] < T(0) ? -1 : 0;
            // On a face, floor picks the upper cell; a ray heading down leaves it at once, so it starts in the lower one.
            if (_step[k] < 0 && grid.plane_of(_cell[k], k) >= start[k])
                --_cell[k];
        }
    }

    // stepping
public:
    /// The next cell along the ray, or none once the ray is past t_max.
    [[nodiscard]] constexpr cc::optional<grid_step<D, T>> next()
    {
        while (!_done)
        {
            // where the ray leaves the current cell, and across which faces
            auto leave = _t_max;
            for (auto k = 0; k < D; ++k)
                if (auto const t = crossing(k); t < leave)
                    leave = t;
            if (!(leave < _t_max))
                _done = true;

            auto const step = grid_step<D, T>{.cell = _cell, .along = {.start = _t, .end = leave}};
            if (!_done)
                for (auto k = 0; k < D; ++k)
                    if (crossing(k) <= leave)
                        _cell[k] += _step[k];
            auto const entered = _t;
            _t = leave;
            // A stretch of no length is a cell the ray only touches: skipped, unless the walk is the one point t_min = t_max.
            if (leave > entered || _is_point)
            {
                _is_point = false;
                return step;
            }
        }
        return {};
    }

    struct sentinel
    {
    };

    class iterator
    {
    public:
        [[nodiscard]] constexpr grid_step<D, T> const& operator*() const { return _current.value(); }
        constexpr iterator& operator++()
        {
            _current = _walk->next();
            return *this;
        }
        [[nodiscard]] friend constexpr bool operator==(iterator const& it, sentinel)
        {
            return !it._current.has_value();
        }

    private:
        friend class grid_traversal;
        constexpr iterator(grid_traversal* walk) : _walk(walk), _current(walk->next()) {}

        grid_traversal* _walk = nullptr;
        cc::optional<grid_step<D, T>> _current;
    };

    [[nodiscard]] constexpr iterator begin() { return iterator(this); }
    [[nodiscard]] constexpr sentinel end() const { return {}; }

private:
    /// Where the ray crosses the face it heads for along `axis`, never before where it is now.
    [[nodiscard]] constexpr T crossing(int axis) const
    {
        if (_step[axis] == 0)
            return tg::traits::highest<T>();
        auto const plane = _grid.plane_of(_step[axis] > 0 ? _cell[axis] + 1 : _cell[axis], axis);
        auto const t = (plane - _ray.origin[axis]) / _ray.dir[axis];
        return t < _t ? _t : t;
    }

    uniform_grid<D, T> _grid;
    ray<D, T> _ray;
    pos<D, i32> _cell;
    i32 _step[D] = {};
    T _t = {};
    T _t_max = {};
    bool _done = false;
    bool _is_point = false;
};
