#pragma once

#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/impl/rotation_storage.hh>
#include <typed-geometry/transform/transform_flags.hh>

namespace tg::impl
{
/// The linear part of a transform, one specialization per canonical linear class.
///
/// The primary is left undefined so that a class this table does not cover is a hard error rather
/// than a silently empty transform.
///
/// Every member carries a default initializer, because vec, mat and quat all default to ZERO —
/// a transform built from their defaults would be singular instead of the identity.
template <int D, class T, transform_flags LinearFlags>
struct linear_storage;

template <int D, class T>
struct linear_storage<D, T, linear_kind::identity>
{
    [[nodiscard]] friend constexpr bool operator==(linear_storage const&, linear_storage const&) = default;
};

template <int D, class T>
struct linear_storage<D, T, linear_kind::uniform_scaling>
{
    T scale = tg::one<T>();

    [[nodiscard]] friend constexpr bool operator==(linear_storage const&, linear_storage const&) = default;
};

template <int D, class T>
struct linear_storage<D, T, linear_kind::scaling>
{
    vec<D, T> scale = vec<D, T>(tg::one<T>());

    [[nodiscard]] friend constexpr bool operator==(linear_storage const&, linear_storage const&) = default;
};

template <int D, class T>
struct linear_storage<D, T, linear_kind::rotation>
{
    rotation_storage<D, T> rot;

    [[nodiscard]] friend constexpr bool operator==(linear_storage const&, linear_storage const&) = default;
};

template <int D, class T>
struct linear_storage<D, T, linear_kind::scaled_rotation>
{
    rotation_storage<D, T> rot;
    T scale = tg::one<T>();

    [[nodiscard]] friend constexpr bool operator==(linear_storage const&, linear_storage const&) = default;
};

template <int D, class T>
struct linear_storage<D, T, linear_kind::general_linear>
{
    mat<D, D, T> linear = tg::impl::make_identity<D, D, T>();

    [[nodiscard]] friend constexpr bool operator==(linear_storage const&, linear_storage const&) = default;
};

/// The whole storage of a transform: one of three layouts, holding the linear part above.
///
/// homogeneous_transform holds exactly one of these as its only non-static data member, which keeps it standard-layout.
/// Splitting the linear part into a base class instead would forfeit that as soon as both halves carry members —
/// that is, for rigid, similarity and affine, the common cases.
template <int D, class T, transform_flags LinearFlags, transform_layout Layout>
struct transform_storage;

template <int D, class T, transform_flags LinearFlags>
struct transform_storage<D, T, LinearFlags, transform_layout::linear_only>
{
    linear_storage<D, T, LinearFlags> linear;

    [[nodiscard]] friend constexpr bool operator==(transform_storage const&, transform_storage const&) = default;
};

template <int D, class T, transform_flags LinearFlags>
struct transform_storage<D, T, LinearFlags, transform_layout::translation_only>
{
    vec<D, T> translation;

    [[nodiscard]] friend constexpr bool operator==(transform_storage const&, transform_storage const&) = default;
};

template <int D, class T, transform_flags LinearFlags>
struct transform_storage<D, T, LinearFlags, transform_layout::linear_and_translation>
{
    linear_storage<D, T, LinearFlags> linear;
    vec<D, T> translation;

    [[nodiscard]] friend constexpr bool operator==(transform_storage const&, transform_storage const&) = default;
};

template <int D, class T, transform_flags LinearFlags>
struct transform_storage<D, T, LinearFlags, transform_layout::projective>
{
    mat<D + 1, D + 1, T> m = tg::impl::make_identity<D + 1, D + 1, T>();

    [[nodiscard]] friend constexpr bool operator==(transform_storage const&, transform_storage const&) = default;
};

} // namespace tg::impl
