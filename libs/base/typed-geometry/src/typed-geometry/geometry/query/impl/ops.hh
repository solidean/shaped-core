#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/scalar/traits.hh>

#include <type_traits>

/// The seam every geometric query dispatches through: one class template per verb, specialized per pair of types.
///
/// A kernel is a specialization with a static `apply`; the primary templates are left undefined, so "this pair has a
/// kernel" is exactly "this specialization is complete", which `has_op` detects.
/// A specialization declared after a verb's generic function is still found when that function is instantiated —
/// the property an overloaded, qualified free function would not have.
///
/// A kernel is written once, in whichever argument order is natural; each symmetric verb also tries the mirror.
/// libs/base/typed-geometry/docs/plans/geometry-query-matrix.md has the ladder each verb walks.

namespace tg::impl
{
/// `a.project_to(b)`: a mapped onto b. Not symmetric.
template <class A, class B>
struct project_op;

/// `a.closest_points_to(b)`: {point of a, point of b}.
template <class A, class B>
struct closest_points_op;

/// `a.distance_sqr_to(b)`, where a closed form beats going through closest points.
template <class A, class B>
struct distance_sqr_op;

/// `a.signed_distance_to(b)`: negative inside b. Not symmetric.
template <class A, class B>
struct signed_distance_op;

/// `a.contains(b)`: every point of b is in a. Not symmetric.
template <class A, class B>
struct contains_op;

/// `a.intersects(b)`.
template <class A, class B>
struct intersects_op;

/// `a.separation_from(b)`, where a closed form beats EPA.
template <class A, class B>
struct separation_op;

/// The farthest point of a bounded convex object along a direction: `apply(obj, dir) -> pos`.
/// It is all GJK and EPA need of an object; a direction of zero length may return any point of it.
template <class Obj>
struct support_op;

template <template <class, class> class Op, class A, class B>
concept has_op = requires(A const& a, B const& b) { Op<A, B>::apply(a, b); };

/// the type is a geometric object, i.e. it has an object_traits; checked first so other probes stay soft.
template <class Obj>
concept is_object = requires { object_traits<Obj>::ambient_dim; };

template <class Obj>
concept has_support
    = is_object<Obj> && requires(Obj const& o, vec<traits::ambient_dim<Obj>, traits::scalar_t<Obj>> const& d) {
          support_op<Obj>::apply(o, d);
      };

/// both objects are bounded convex sets with supports in one space, over a scalar GJK may iterate on.
template <class A, class B>
concept gjk_pair = has_support<A> && has_support<B> && traits::ambient_dim<A> == traits::ambient_dim<B>
                && !tg::traits::is_exact<traits::scalar_t<A>>;

/// EPA serves full-dimensional solids only: a flat difference A - B has no interior to be deep inside of.
template <class A, class B>
concept epa_pair = gjk_pair<A, B> && traits::intrinsic_dim<A> == traits::ambient_dim<A>
                && traits::intrinsic_dim<B> == traits::ambient_dim<B>
                && (traits::ambient_dim<A> == 2 || traits::ambient_dim<A> == 3);

/// a boundary type: it has a `.solid()` reading, which every boundary does and nothing else.
template <class Obj>
concept is_boundary = requires(Obj const& o) { o.solid(); };

template <class Obj>
using solid_t = decltype(static_cast<Obj const*>(nullptr)->solid());

template <class T>
inline constexpr bool is_pos = false;
template <int D, class T>
inline constexpr bool is_pos<tg::pos<D, T>> = true;

/// A pair with a kernel in both orders would have its mirror silently shadowed, so it is refused.
template <template <class, class> class Op, class A, class B>
inline constexpr bool written_once = std::is_same_v<A, B> || !(has_op<Op, A, B> && has_op<Op, B, A>);
} // namespace tg::impl

/// The capability concepts: true exactly when a verb has a kernel or a derivation for the pair.
/// They are how generic code asks what a member would answer, since calling one for an unsupported pair is a static_assert.
namespace tg
{
template <class A, class B>
concept has_project_to = impl::has_op<impl::project_op, A, B>;

template <class A, class B>
concept has_closest_points_to = impl::has_op<impl::closest_points_op, A, B> //
                             || impl::has_op<impl::closest_points_op, B, A> //
                             || (impl::is_pos<A> && has_project_to<A, B>)   //
                             || (impl::is_pos<B> && has_project_to<B, A>) || impl::gjk_pair<A, B>;

template <class A, class B>
concept has_distance_sqr_to = impl::has_op<impl::distance_sqr_op, A, B> //
                           || impl::has_op<impl::distance_sqr_op, B, A> || has_closest_points_to<A, B>;

template <class A, class B>
concept has_signed_distance_to = impl::has_op<impl::signed_distance_op, A, B>;

template <class A, class B>
concept has_contains = impl::has_op<impl::contains_op, A, B> //
                    || (impl::is_pos<B> && has_project_to<B, A>);

/// the rungs of intersects that do not go through a boundary's solid.
template <class A, class B>
concept has_intersects_direct = impl::has_op<impl::intersects_op, A, B> //
                             || impl::has_op<impl::intersects_op, B, A> //
                             || (impl::is_pos<A> && has_contains<B, A>) //
                             || (impl::is_pos<B> && has_contains<A, B>) || impl::gjk_pair<A, B>;

template <class A, class B>
concept has_intersects
    = has_intersects_direct<A, B> //
   || (impl::is_boundary<A> && has_intersects_direct<impl::solid_t<A>, B> && has_contains<impl::solid_t<A>, B>)
   || (impl::is_boundary<B> && has_intersects_direct<impl::solid_t<B>, A> && has_contains<impl::solid_t<B>, A>);

template <class A, class B>
concept has_separation_from = impl::has_op<impl::separation_op, A, B> || impl::epa_pair<A, B>;
} // namespace tg
