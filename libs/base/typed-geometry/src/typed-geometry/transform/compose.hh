#pragma once

#include <typed-geometry/transform/composed_transform.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// the transform that applies `b` first, then `a`.
///
/// There is deliberately no operator*: a transform is applied, not multiplied.
///
/// Fusing wins when it is available: if `a` can absorb `b` — which every homogeneous_transform can, for
/// every class of its own dimension and scalar — the result is ONE transform of the join class, and an
/// object pays for a single application.
/// Otherwise the two are kept side by side in a tg::composed_transform, so composing is total even across
/// transform types that know nothing about each other.
///
/// The choice is made at compile time, so the return type says which one you got.
template <class TransformA, class TransformB>
[[nodiscard]] constexpr auto compose(TransformA const& a, TransformB const& b)
{
    if constexpr (requires { a.composed(b); })
        return a.composed(b);
    else
        return composed_transform<TransformA, TransformB>(a, b);
}

} // namespace tg
