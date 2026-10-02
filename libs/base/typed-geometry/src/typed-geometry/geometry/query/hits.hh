#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/geometry/fwd.hh>

/// Where a line, ray or segment meets a surface: at most N hits, sorted along it.
///
/// A hit is a parameter of the linear object (`r.at(t)` is the point), or a point when `intersection_with` asks for
/// the points themselves; either way the order is the order along the linear object.
/// Fixed capacity, no allocation: a sphere's surface is crossed at most twice, so `hits<2, T>`.
template <int N, class HitT>
struct tg::hits
{
    static_assert(N > 0, "hits needs a positive capacity");

    static constexpr int capacity = N;

    // construction
public:
    hits() = default;

    /// appends a hit; hits must be added in order along the linear object.
    constexpr void add(HitT const& h)
    {
        CC_ASSERT(_size < N, "more hits than the capacity the kernel declared");
        _hits[_size++] = h;
    }

    // access
public:
    [[nodiscard]] constexpr int size() const { return _size; }
    [[nodiscard]] constexpr bool has_any() const { return _size > 0; }
    [[nodiscard]] constexpr bool is_empty() const { return _size == 0; }

    /// the nearest hit; there must be one.
    [[nodiscard]] constexpr HitT const& first() const
    {
        CC_ASSERT(_size > 0, "no hit");
        return _hits[0];
    }
    /// the farthest hit; there must be one.
    [[nodiscard]] constexpr HitT const& last() const
    {
        CC_ASSERT(_size > 0, "no hit");
        return _hits[_size - 1];
    }
    [[nodiscard]] constexpr HitT const& operator[](int i) const
    {
        CC_ASSERT(0 <= i && i < _size, "hit index out of range");
        return _hits[i];
    }

    [[nodiscard]] constexpr HitT const* begin() const { return _hits; }
    [[nodiscard]] constexpr HitT const* end() const { return _hits + _size; }

    /// the same hits mapped through `f` — parameters to points, say — keeping their order.
    template <class F>
    [[nodiscard]] constexpr auto mapped(F&& f) const
    {
        hits<N, decltype(f(_hits[0]))> r;
        for (int i = 0; i < _size; ++i)
            r.add(f(_hits[i]));
        return r;
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(hits const& a, hits const& b)
    {
        if (a._size != b._size)
            return false;
        for (int i = 0; i < a._size; ++i)
            if (!(a._hits[i] == b._hits[i]))
                return false;
        return true;
    }

private:
    HitT _hits[N] = {};
    int _size = 0;
};

/// The part of a line, ray or segment inside a solid: the parameters [start, end], start <= end.
/// An end the solid does not bound (a line through a half-space) is the scalar's infinity, or its largest value.
template <class T>
struct tg::hit_interval
{
    T start = {};
    T end = {};

    [[nodiscard]] friend constexpr bool operator==(hit_interval const&, hit_interval const&) = default;
};
