#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/linalg/mat_ops.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// the inverse transform, which runs from the target space back to the source space.
///
/// Every canonical transform class is closed under inversion, so the class is unchanged: a rotation
/// inverts to a rotation, a signed uniform scale to a signed uniform scale, a rigid map to a rigid map.
///
/// No scale factor may be zero.
/// A singular linear or homogeneous matrix is tolerated instead: it inverts to the zero matrix, following tg::inverse(mat).
template <int DSource, int DTarget, class T, transform_flags F>
[[nodiscard]] constexpr homogeneous_transform<DTarget, DSource, T, F> inverse(
    homogeneous_transform<DSource, DTarget, T, F> const& t)
{
    using transform_t = homogeneous_transform<DTarget, DSource, T, F>;
    constexpr auto lf = transform_t::linear_flags;

    transform_t result;

    if constexpr (transform_t::has_projection)
    {
        result.storage.m = tg::inverse(t.storage.m);
        return result;
    }
    else
    {
        if constexpr (lf == tg::impl::linear_kind::general_linear)
            result.storage.linear.linear = tg::inverse(t.storage.linear.linear);
        else if constexpr (lf == tg::impl::linear_kind::scaled_rotation)
        {
            CC_ASSERT(!tg::traits::is_zero(t.storage.linear.scale), "cannot invert a transform with a zero scale");
            result.storage.linear.rot = t.storage.linear.rot.inverse();
            result.storage.linear.scale = tg::one<T>() / t.storage.linear.scale;
        }
        else if constexpr (lf == tg::impl::linear_kind::rotation)
            result.storage.linear.rot = t.storage.linear.rot.inverse();
        else if constexpr (lf == tg::impl::linear_kind::scaling)
        {
            for (int i = 0; i < DTarget; ++i)
            {
                CC_ASSERT(!tg::traits::is_zero(t.storage.linear.scale.data[i]), "cannot invert a transform with a zero "
                                                                                "scale");
                result.storage.linear.scale.data[i] = tg::one<T>() / t.storage.linear.scale.data[i];
            }
        }
        else if constexpr (lf == tg::impl::linear_kind::uniform_scaling)
        {
            CC_ASSERT(!tg::traits::is_zero(t.storage.linear.scale), "cannot invert a transform with a zero scale");
            result.storage.linear.scale = tg::one<T>() / t.storage.linear.scale;
        }

        if constexpr (transform_t::has_translation)
            result.storage.translation = -result.apply_linear(t.storage.translation);

        return result;
    }
}

} // namespace tg
