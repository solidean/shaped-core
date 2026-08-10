#pragma once

#include <clean-core/common/flags.hh>
#include <typed-geometry/scalar/fwd.hh>

namespace tg::impl
{
/// What a transform is allowed to contain.
/// Bit flags — combine with `|`, and hold the result in a cc::flags<transform_flags>, which is what every function here takes.
///
/// These select the capability class of tg::homogeneous_transform, which picks its representation from them and never exposes it.
/// This is machinery, not API: name a class through one of the tg aliases (tg::rigid_transform3f, tg::affine_transform<D, T>, ...)
/// rather than reaching in here, and note that the transform type requires its flags to be canonical.
///
///     auto const r = tg::rigid_transform3f::make_rotation_y(90_deg_f);
///     auto const a = tg::affine_transform3f(r);   // explicit widening, always lossless
///
/// Scaling factors are POSITIVE unless `negative_scaling` says otherwise.
/// That is what lets narrow classes stay simple: an aabb under a positive scaling keeps its min and max in order,
/// and a sphere's radius is just multiplied by the factor.
enum class transform_flags : u32
{
    none = 0,

    translation = 1u << 0,         // an additive offset
    uniform_scaling = 1u << 1,     // one factor applied to every axis
    non_uniform_scaling = 1u << 2, // one factor per axis
    negative_scaling = 1u << 3,    // a scale factor may be negative, so the map may reverse orientation
    rotation = 1u << 4,            // a proper rotation, SO(D)
    general_linear = 1u << 5,      // an arbitrary invertible linear part, GL(D)
    projection = 1u << 6,          // a non-trivial homogeneous bottom row

    all = (1u << 7) - 1,
};
} // namespace tg::impl

CC_FLAG_ENUM(tg::impl, transform_flags, u32);

namespace tg::impl
{
/// Reduce a flag set to the canonical representative of its transform class.
///
/// Several bit patterns denote the same set of transforms, and each such set must have exactly one type.
/// The rules, in an order that reaches a fixpoint in one pass:
///
///   1. a projective transform contains every affine one
///   2. rotation together with non-uniform scaling is not closed under composition: R1 S1 R2 S2 is a general matrix.
///      This is exact, not conservative — every invertible A has an SVD A = U S V^T,
///      so the closure of SO(D) and the signed diagonals is all of GL(D)
///   3. a general linear part is any invertible matrix, so it already contains rotations, scalings and the orientation-reversing ones.
///      tg does not model GL+(D) separately.
///   4. non-uniform scaling subsumes uniform scaling
///   5. a negative factor needs a factor to sit on, so the flag means nothing without scaling
///
/// There are 19 canonical classes: nine linear ones with and without translation, plus projective.
[[nodiscard]] constexpr cc::flags<transform_flags> transform_canonical(cc::flags<transform_flags> f)
{
    if (f.has_any(transform_flags::projection))
        return transform_flags::all;

    if (f.has_all(transform_flags::rotation | transform_flags::non_uniform_scaling))
        f |= transform_flags::general_linear;

    if (f.has_any(transform_flags::general_linear))
        f |= transform_flags::rotation | transform_flags::non_uniform_scaling | transform_flags::negative_scaling;

    if (f.has_any(transform_flags::non_uniform_scaling))
        f = f.without(transform_flags::uniform_scaling);

    if (!f.has_any(transform_flags::uniform_scaling | transform_flags::non_uniform_scaling))
        f = f.without(transform_flags::negative_scaling);

    return f;
}

/// is this flag set already the representative of its class?
[[nodiscard]] constexpr bool transform_is_canonical(cc::flags<transform_flags> f)
{
    return tg::impl::transform_canonical(f) == f;
}

/// true if every transform of class `sub` is also a transform of class `super`.
/// `super` must be canonical.
///
/// This is NOT a bit-subset test, and using has_all here is the module's easiest mistake.
/// transform_canonical() CLEARS bits — affine drops uniform_scaling because non_uniform_scaling subsumes it —
/// so has_all(affine, similarity) is false even though every similarity is affine.
/// transform_canonical(a | b) is the join in the class lattice, so containment is "the join is already `super`".
[[nodiscard]] constexpr bool transform_is_subclass(cc::flags<transform_flags> sub, cc::flags<transform_flags> super)
{
    return tg::impl::transform_canonical(sub | super) == super;
}

/// The canonical transform classes, by name.
/// Every name here is canonical, which is what homogeneous_transform requires of its flag argument.
namespace transform_class
{
// linear only; scale factors are positive
inline constexpr cc::flags<transform_flags> identity = transform_flags::none;
inline constexpr cc::flags<transform_flags> uniform_scaling = transform_flags::uniform_scaling;
inline constexpr cc::flags<transform_flags> scaling = transform_flags::non_uniform_scaling;
inline constexpr cc::flags<transform_flags> rotation = transform_flags::rotation;
inline constexpr cc::flags<transform_flags> scaled_rotation
    = tg::impl::transform_canonical(transform_flags::rotation | transform_flags::uniform_scaling);

/// any invertible linear map, so orientation-reversing ones included — see canonicalization rule 3.
inline constexpr cc::flags<transform_flags> linear = tg::impl::transform_canonical(transform_flags::general_linear);

// with a translation
inline constexpr cc::flags<transform_flags> translation = transform_flags::translation;
inline constexpr cc::flags<transform_flags> uniform_scaling_translation
    = tg::impl::transform_canonical(uniform_scaling | translation);
inline constexpr cc::flags<transform_flags> scaling_translation = tg::impl::transform_canonical(scaling | translation);
inline constexpr cc::flags<transform_flags> rigid = tg::impl::transform_canonical(rotation | translation);
inline constexpr cc::flags<transform_flags> similarity = tg::impl::transform_canonical(scaled_rotation | translation);
inline constexpr cc::flags<transform_flags> affine = tg::impl::transform_canonical(linear | translation);

/// The same classes with a signed scale factor, so they may reverse orientation.
///
/// `signed_similarity` is the one that earns its keep: in 3D a negative uniform scale composed with a
/// half-turn is a plane reflection, so it is the FULL conformal group — and a sphere still maps to a
/// sphere under it, only the radius takes the magnitude.
inline constexpr cc::flags<transform_flags> signed_uniform_scaling
    = tg::impl::transform_canonical(uniform_scaling | transform_flags::negative_scaling);
inline constexpr cc::flags<transform_flags> signed_scaling
    = tg::impl::transform_canonical(scaling | transform_flags::negative_scaling);
inline constexpr cc::flags<transform_flags> signed_scaled_rotation
    = tg::impl::transform_canonical(scaled_rotation | transform_flags::negative_scaling);
inline constexpr cc::flags<transform_flags> signed_uniform_scaling_translation
    = tg::impl::transform_canonical(uniform_scaling_translation | transform_flags::negative_scaling);
inline constexpr cc::flags<transform_flags> signed_scaling_translation
    = tg::impl::transform_canonical(scaling_translation | transform_flags::negative_scaling);
inline constexpr cc::flags<transform_flags> signed_similarity
    = tg::impl::transform_canonical(similarity | transform_flags::negative_scaling);

// the top of the lattice
inline constexpr cc::flags<transform_flags> projective = transform_flags::all;
} // namespace transform_class

/// the linear part of a class, as far as the REPRESENTATION is concerned.
///
/// negative_scaling is dropped along with translation and projection: whether a factor may be
/// negative is a promise about its value, not a change of layout, so a signed similarity stores
/// exactly what a similarity stores.
[[nodiscard]] constexpr cc::flags<transform_flags> linear_part(cc::flags<transform_flags> f)
{
    return f.without(transform_flags::translation | transform_flags::projection | transform_flags::negative_scaling);
}

/// The six representation kinds — the canonical linear classes with negative_scaling stripped.
///
/// linear_part() of any class is one of these, so they are what the representation table specializes on and
/// what the `if constexpr` chains compare against.
/// Do not compare linear_part() against transform_class::* directly: transform_class::linear carries
/// negative_scaling, so it is NOT equal to its own linear part.
namespace linear_kind
{
inline constexpr cc::flags<transform_flags> identity = tg::impl::linear_part(transform_class::identity);
inline constexpr cc::flags<transform_flags> uniform_scaling = tg::impl::linear_part(transform_class::uniform_scaling);
inline constexpr cc::flags<transform_flags> scaling = tg::impl::linear_part(transform_class::scaling);
inline constexpr cc::flags<transform_flags> rotation = tg::impl::linear_part(transform_class::rotation);
inline constexpr cc::flags<transform_flags> scaled_rotation = tg::impl::linear_part(transform_class::scaled_rotation);
inline constexpr cc::flags<transform_flags> general_linear = tg::impl::linear_part(transform_class::linear);
} // namespace linear_kind

/// how a class lays out its representation.
///
/// A pure translation gets its own layout rather than an identity linear part next to a vector:
/// an empty member still occupies a byte, which alignment would then round up to a whole scalar.
enum class transform_layout
{
    linear_only,
    translation_only,
    linear_and_translation,
    projective,
};

[[nodiscard]] constexpr transform_layout layout_of(cc::flags<transform_flags> f)
{
    if (f.has_any(transform_flags::projection))
        return transform_layout::projective;

    if (f.has_any(transform_flags::translation))
        return tg::impl::linear_part(f) == transform_class::identity ? transform_layout::translation_only
                                                                     : transform_layout::linear_and_translation;

    return transform_layout::linear_only;
}

} // namespace tg::impl
