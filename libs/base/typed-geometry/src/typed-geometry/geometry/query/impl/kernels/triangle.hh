#pragma once

#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/vec_ops.hh>

/// The nearest point of a filled triangle, in any dimension.
///
/// The plane of the triangle splits into seven regions — three vertices, three edges, the face — and which one p's
/// foot falls in is decided from dot products against the two edges at each vertex alone, so nothing here needs
/// a normal and the same code serves 2D and 3D.
/// The face case reads the barycentrics off the same products; a zero-area triangle makes that division the special case.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::triangle<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, triangle<D, T> const& t)
    {
        auto const& a = t.pos0;
        auto const& b = t.pos1;
        auto const& c = t.pos2;
        auto const ab = b - a;
        auto const ac = c - a;

        // vertex a: p lies behind both edges leaving a
        auto const ap = p - a;
        auto const d1 = tg::dot(ab, ap);
        auto const d2 = tg::dot(ac, ap);
        if (d1 <= T(0) && d2 <= T(0))
            return a;

        // vertex b
        auto const bp = p - b;
        auto const d3 = tg::dot(ab, bp);
        auto const d4 = tg::dot(ac, bp);
        if (d3 >= T(0) && d4 <= d3)
            return b;

        // edge ab: between a and b along ab, and outside across ab
        auto const vc = d1 * d4 - d3 * d2;
        if (vc <= T(0) && d1 >= T(0) && d3 <= T(0))
            return a + ab * (d1 / (d1 - d3));

        // vertex c
        auto const cp = p - c;
        auto const d5 = tg::dot(ab, cp);
        auto const d6 = tg::dot(ac, cp);
        if (d6 >= T(0) && d5 <= d6)
            return c;

        // edge ac
        auto const vb = d5 * d2 - d1 * d6;
        if (vb <= T(0) && d2 >= T(0) && d6 <= T(0))
            return a + ac * (d2 / (d2 - d6));

        // edge bc
        auto const va = d3 * d6 - d5 * d4;
        if (va <= T(0) && d4 - d3 >= T(0) && d5 - d6 >= T(0))
            return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

        // the face: va, vb, vc are the barycentrics scaled by twice the squared area
        auto const sum = va + vb + vc;
        TG_SPECIAL_CASE(tg::traits::is_zero(sum), "projection onto a zero-area triangle");
        return a + ab * (vb / sum) + ac * (vc / sum);
    }
};

template <int D, class T>
struct tg::impl::support_op<tg::triangle<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(triangle<D, T> const& t, vec<D, T> const& dir)
    {
        auto const d0 = tg::dot(t.pos0 - pos<D, T>(), dir);
        auto const d1 = tg::dot(t.pos1 - pos<D, T>(), dir);
        auto const d2 = tg::dot(t.pos2 - pos<D, T>(), dir);
        if (d0 >= d1 && d0 >= d2)
            return t.pos0;
        return d1 >= d2 ? t.pos1 : t.pos2;
    }
};
