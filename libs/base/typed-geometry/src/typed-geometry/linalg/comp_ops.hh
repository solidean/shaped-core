#pragma once

#include <typed-geometry/linalg/comp.hh>

/// Additional component-wise operations on comp that read better as free functions. Like the
/// operators in comp.hh these are fully element-wise, with scalar operands broadcasting.

namespace tg
{
/// component-wise minimum.
template <int D, class T>
[[nodiscard]] constexpr comp<D, T> min(comp<D, T> const& a, comp<D, T> const& b)
{
    comp<D, T> r;
    for (int i = 0; i < D; ++i)
        r.data[i] = a.data[i] < b.data[i] ? a.data[i] : b.data[i];
    return r;
}
/// component-wise maximum.
template <int D, class T>
[[nodiscard]] constexpr comp<D, T> max(comp<D, T> const& a, comp<D, T> const& b)
{
    comp<D, T> r;
    for (int i = 0; i < D; ++i)
        r.data[i] = a.data[i] < b.data[i] ? b.data[i] : a.data[i];
    return r;
}

/// component-wise minimum against a broadcast scalar bound.
template <int D, class T>
[[nodiscard]] constexpr comp<D, T> min(comp<D, T> const& a, T s)
{
    comp<D, T> r;
    for (int i = 0; i < D; ++i)
        r.data[i] = a.data[i] < s ? a.data[i] : s;
    return r;
}
/// component-wise maximum against a broadcast scalar bound.
template <int D, class T>
[[nodiscard]] constexpr comp<D, T> max(comp<D, T> const& a, T s)
{
    comp<D, T> r;
    for (int i = 0; i < D; ++i)
        r.data[i] = a.data[i] < s ? s : a.data[i];
    return r;
}
} // namespace tg
