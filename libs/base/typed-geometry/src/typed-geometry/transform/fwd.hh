#pragma once

#include <typed-geometry/scalar/fwd.hh>
#include <typed-geometry/transform/transform_flags.hh>

namespace tg
{
/// a transform of the capability class `Flags`, mapping DSource-dimensional space to DTarget-dimensional space.
/// Flags must be canonical; the representation is chosen from them and is not part of the API.
///
/// The two dimensions are what lets a transform lift or project between spaces.
/// Only the square case is implemented today — the type asserts DSource == DTarget.
template <int DSource, int DTarget, class T, cc::flags<tg::impl::transform_flags> Flags>
struct homogeneous_transform;

/// two arbitrary transforms held side by side, applied inner first — what tg::compose falls back to.
template <class TransformOuter, class TransformInner>
struct composed_transform;

//
// Class aliases
//
// Every one of these is SQUARE: it names a transform of a space onto itself, which is what almost all code wants.
// Spell homogeneous_transform directly for a map between two different dimensions.
//
// The flag argument is a constant, so these aliases stay transparent: template argument deduction and partial specialization both see through them.
// An alias whose flag argument were *computed* (see transform_for below) would put Flags in a non-deduced context instead.
//

template <int D, class T>
using identity_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::identity>;
template <int D, class T>
using translation_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::translation>;
template <int D, class T>
using rotation_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::rotation>;
template <int D, class T>
using scaling_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::scaling>;
template <int D, class T>
using scaling_translation_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::scaling_translation>;
template <int D, class T>
using linear_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::linear>;
template <int D, class T>
using rigid_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::rigid>;
template <int D, class T>
using similarity_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::similarity>;

// the signed variants, which may reverse orientation
template <int D, class T>
using signed_scaling_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::signed_scaling>;
template <int D, class T>
using signed_scaling_translation_transform
    = homogeneous_transform<D, D, T, tg::impl::transform_class::signed_scaling_translation>;
template <int D, class T>
using signed_similarity_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::signed_similarity>;
template <int D, class T>
using affine_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::affine>;
template <int D, class T>
using projective_transform = homogeneous_transform<D, D, T, tg::impl::transform_class::projective>;

/// canonicalizing spelling, for return types and other non-deduced positions only.
/// Never use this in a function parameter or a partial specialization — Flags would not deduce.
template <int DSource, int DTarget, class T, cc::flags<tg::impl::transform_flags> Flags>
using transform_for = homogeneous_transform<DSource, DTarget, T, tg::impl::transform_canonical(Flags)>;

//
// Concrete typedefs (suffix: f = f32, d = f64)
//

using identity_transform2f = identity_transform<2, f32>;
using identity_transform3f = identity_transform<3, f32>;
using identity_transform2d = identity_transform<2, f64>;
using identity_transform3d = identity_transform<3, f64>;

using translation_transform2f = translation_transform<2, f32>;
using translation_transform3f = translation_transform<3, f32>;
using translation_transform2d = translation_transform<2, f64>;
using translation_transform3d = translation_transform<3, f64>;

using rotation_transform2f = rotation_transform<2, f32>;
using rotation_transform3f = rotation_transform<3, f32>;
using rotation_transform2d = rotation_transform<2, f64>;
using rotation_transform3d = rotation_transform<3, f64>;

using scaling_transform2f = scaling_transform<2, f32>;
using scaling_transform3f = scaling_transform<3, f32>;
using scaling_transform2d = scaling_transform<2, f64>;
using scaling_transform3d = scaling_transform<3, f64>;

using scaling_translation_transform2f = scaling_translation_transform<2, f32>;
using scaling_translation_transform3f = scaling_translation_transform<3, f32>;
using scaling_translation_transform2d = scaling_translation_transform<2, f64>;
using scaling_translation_transform3d = scaling_translation_transform<3, f64>;

using linear_transform2f = linear_transform<2, f32>;
using linear_transform3f = linear_transform<3, f32>;
using linear_transform2d = linear_transform<2, f64>;
using linear_transform3d = linear_transform<3, f64>;

using rigid_transform2f = rigid_transform<2, f32>;
using rigid_transform3f = rigid_transform<3, f32>;
using rigid_transform2d = rigid_transform<2, f64>;
using rigid_transform3d = rigid_transform<3, f64>;

using similarity_transform2f = similarity_transform<2, f32>;
using similarity_transform3f = similarity_transform<3, f32>;
using similarity_transform2d = similarity_transform<2, f64>;
using similarity_transform3d = similarity_transform<3, f64>;

using signed_scaling_transform2f = signed_scaling_transform<2, f32>;
using signed_scaling_transform3f = signed_scaling_transform<3, f32>;
using signed_scaling_transform2d = signed_scaling_transform<2, f64>;
using signed_scaling_transform3d = signed_scaling_transform<3, f64>;

using signed_scaling_translation_transform2f = signed_scaling_translation_transform<2, f32>;
using signed_scaling_translation_transform3f = signed_scaling_translation_transform<3, f32>;
using signed_scaling_translation_transform2d = signed_scaling_translation_transform<2, f64>;
using signed_scaling_translation_transform3d = signed_scaling_translation_transform<3, f64>;

using signed_similarity_transform2f = signed_similarity_transform<2, f32>;
using signed_similarity_transform3f = signed_similarity_transform<3, f32>;
using signed_similarity_transform2d = signed_similarity_transform<2, f64>;
using signed_similarity_transform3d = signed_similarity_transform<3, f64>;

using affine_transform2f = affine_transform<2, f32>;
using affine_transform3f = affine_transform<3, f32>;
using affine_transform2d = affine_transform<2, f64>;
using affine_transform3d = affine_transform<3, f64>;

using projective_transform2f = projective_transform<2, f32>;
using projective_transform3f = projective_transform<3, f32>;
using projective_transform2d = projective_transform<2, f64>;
using projective_transform3d = projective_transform<3, f64>;

} // namespace tg
