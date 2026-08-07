#pragma once

#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// Test-only approximate comparison helpers (nexus has no built-in approx).

namespace tgtest
{
template <class T>
[[nodiscard]] inline bool approx(T a, T b, T eps = T(1e-4))
{
    T const d = a - b;
    return -eps < d && d < eps;
}

template <int D, class T>
[[nodiscard]] inline bool approx(tg::vec<D, T> const& a, tg::vec<D, T> const& b, T eps = T(1e-4))
{
    for (int i = 0; i < D; ++i)
        if (!approx(a.data[i], b.data[i], eps))
            return false;
    return true;
}

template <int D, class T>
[[nodiscard]] inline bool approx(tg::pos<D, T> const& a, tg::pos<D, T> const& b, T eps = T(1e-4))
{
    for (int i = 0; i < D; ++i)
        if (!approx(a.data[i], b.data[i], eps))
            return false;
    return true;
}

template <int C, int R, class T>
[[nodiscard]] inline bool approx(tg::mat<C, R, T> const& a, tg::mat<C, R, T> const& b, T eps = T(1e-4))
{
    for (int c = 0; c < C; ++c)
        if (!approx(a.cols[c], b.cols[c], eps))
            return false;
    return true;
}
} // namespace tgtest
