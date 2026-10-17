#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

namespace tg::impl
{
template <int D, class T>
[[nodiscard]] constexpr pos<D, T> filled_pos(T v)
{
    auto p = pos<D, T>();
    for (auto i = 0; i < D; ++i)
        p.data[i] = v;
    return p;
}

template <class R>
concept bounds_range = requires(R const& r) {
    r.begin();
    r.end();
} || std::is_array_v<R>;

/// The aabb type an argument of aabb_of contributes to: a pos's own, a range's element's, an object's bounds().
template <class Arg>
struct bounds_aabb;

template <int D, class T>
struct bounds_aabb<pos<D, T>>
{
    using type = aabb<D, T>;
};

template <class Arg>
    requires std::is_array_v<Arg>
struct bounds_aabb<Arg>
{
    using type = typename bounds_aabb<std::remove_cvref_t<std::remove_extent_t<Arg>>>::type;
};

template <class Arg>
    requires(bounds_range<Arg> && !std::is_array_v<Arg>)
struct bounds_aabb<Arg>
{
    using type = typename bounds_aabb<std::remove_cvref_t<decltype(*std::declval<Arg const&>().begin())>>::type;
};

template <class Arg>
    requires(!bounds_range<Arg> && requires(Arg const& a) { a.bounds(); })
struct bounds_aabb<Arg>
{
    using type = std::remove_cvref_t<decltype(std::declval<Arg const&>().bounds())>;
};

/// Whether Arg is a tg::pos, and of which dimension.
template <class Arg>
struct pos_shape
{
    static constexpr bool is_pos = false;
    static constexpr int dim = 0;
};

template <int D, class T>
struct pos_shape<pos<D, T>>
{
    static constexpr bool is_pos = true;
    static constexpr int dim = D;
};

/// Grows `box` to hold `arg`: a point, an object through its bounds(), or every element of a range.
/// Coordinates of another scalar convert to the box's, which is what aabb<D, T>::of naming the box type is for.
template <int D, class T, class Arg>
constexpr void grow_bounds(aabb<D, T>& box, Arg const& arg)
{
    if constexpr (pos_shape<Arg>::is_pos)
    {
        static_assert(pos_shape<Arg>::dim == D, "tg::aabb::of needs points of the box's dimension");
        for (auto i = 0; i < D; ++i)
        {
            auto const v = T(arg.data[i]);
            if (v < box.min.data[i])
                box.min.data[i] = v;
            if (v > box.max.data[i])
                box.max.data[i] = v;
        }
    }
    else if constexpr (bounds_range<Arg>)
    {
        for (auto const& element : arg)
            grow_bounds(box, element);
    }
    else if constexpr (requires { arg.bounds(); })
    {
        // the empty box is the identity here: its +inf / -inf extremes never win a comparison
        auto const b = arg.bounds();
        static_assert(pos_shape<std::remove_cvref_t<decltype(b.min)>>::dim == D,
                      "tg::aabb::of needs objects whose bounds() is an aabb of the box's dimension");
        for (auto i = 0; i < D; ++i)
        {
            auto const lo = T(b.min.data[i]);
            auto const hi = T(b.max.data[i]);
            if (lo < box.min.data[i])
                box.min.data[i] = lo;
            if (hi > box.max.data[i])
                box.max.data[i] = hi;
        }
    }
    else
        static_assert(false, "tg::aabb_of takes points, objects with bounds() and ranges of either; an unbounded "
                             "object has no bounds()");
}

/// the smallest aabb holding every point given.
template <int D, class T, class... Ps>
[[nodiscard]] constexpr aabb<D, T> bounds_of(pos<D, T> const& first, Ps const&... rest)
{
    auto r = aabb<D, T>(first, first);
    auto const grow = [&](pos<D, T> const& p)
    {
        for (int i = 0; i < D; ++i)
        {
            if (p.data[i] < r.min.data[i])
                r.min.data[i] = p.data[i];
            if (p.data[i] > r.max.data[i])
                r.max.data[i] = p.data[i];
        }
    };
    (grow(rest), ...);
    return r;
}

/// per coordinate, how far a disk of radius r perpendicular to the unit-free axis u reaches: r * sqrt(1 - u_k^2 / |u|^2).
template <class T>
[[nodiscard]] constexpr vec<3, T> disk_reach(vec<3, T> const& u, T r)
{
    auto const uu = tg::dot(u, u);
    vec<3, T> e;
    for (int k = 0; k < 3; ++k)
    {
        auto const s = T(1) - u.data[k] * u.data[k] / uu;
        e.data[k] = r * tg::sqrt(s > T(0) ? s : T(0));
    }
    return e;
}
} // namespace tg::impl
