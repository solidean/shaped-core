#pragma once

#include <typed-geometry/transform/fwd.hh>
#include <typed-geometry/transform/impl/transform_representation.hh>

namespace tg::impl
{
/// the members a transform is actually made of, for code that can beat the public accessors by reading them.
///
/// The accessors answer in geometric terms and pay a conversion wherever the class does not store that form —
/// linear_mat() on a rigid transform builds a matrix out of a quaternion, for instance.
/// An object that knows the layout can take the shorter route through here instead.
///
/// The layout follows the class and is not API: it changes whenever the class does, so a caller must branch on
/// linear_part(Flags) exactly as the transform itself does.
///
/// homogeneous_transform befriends this, which is why the declaration lives in its own header:
/// the friend declaration needs it, and the transform's own file should open with the transform.
template <int DSource, int DTarget, class T, transform_flags Flags>
[[nodiscard]] constexpr transform_representation<DSource, T, linear_part(Flags), layout_of(Flags)> const&
transform_representation_of(homogeneous_transform<DSource, DTarget, T, Flags> const& t)
{
    return t._representation;
}
} // namespace tg::impl
