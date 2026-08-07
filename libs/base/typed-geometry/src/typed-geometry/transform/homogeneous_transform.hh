#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/linalg/bivec.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/transform/fwd.hh>
#include <typed-geometry/transform/impl/transform_representation.hh>
#include <typed-geometry/transform/impl/transform_representation_access.hh>

namespace tg
{
/// A transform of the capability class `Flags`, mapping DSource-dimensional space to DTarget-dimensional space.
///
/// The two dimensions are what will let a transform lift or project between spaces: `pos<DSource, T>` goes in and
/// `pos<DTarget, T>` comes out, the linear part is a `mat<DSource, DTarget, T>` and the translation lives in the
/// TARGET space.
/// Only the square case is implemented today, and the type asserts it — the parameter is in place, the mixed-dimension
/// maths is not.
/// Every named alias (tg::rigid_transform3f, tg::affine_transform<D, T>, ...) is square, so almost no caller sees the second parameter.
///
/// The class picks the representation — a translation is a vec, a rotation a quat, a general linear part a mat — and it is private.
/// Read a transform back through translation(), rotation(), scale(), linear_mat() or to_mat();
/// tg::impl::transform_representation_of is the deliberate back door for code that needs the members themselves.
///
/// Default construction yields the IDENTITY, not a zero-filled transform.
///
///     auto const r = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_y(90_deg_f));
///     auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(0, 1, 0));
///     auto const rt = t.composed(r);                  // applies r first, then t
///     auto const a = tg::affine_transform3f(rt);      // widening is explicit and lossless
///
/// Applying and composing agree, by construction:
///
///     t(r(obj))  ==  t.composed(r).transform(obj)  ==  obj.transformed(r).transformed(t)
///
/// Flags must be canonical — use one of the aliases (tg::rigid_transform3f, tg::affine_transform3f, ...) rather than
/// spelling the flag argument.
///
/// Equality is representational, not geometric: a quaternion and its negation denote the same
/// rotation but do not compare equal, exactly as for tg::quat.
template <int DSource, int DTarget, class T, tg::impl::transform_flags Flags>
struct homogeneous_transform
{
    static_assert(DSource == 2 || DSource == 3,
                  "typed-geometry transforms are 2D or 3D; the 4x4 case is the 3D homogeneous matrix");
    static_assert(DTarget == 2 || DTarget == 3,
                  "typed-geometry transforms are 2D or 3D; the 4x4 case is the 3D homogeneous matrix");
    static_assert(DSource == DTarget,
                  "a transform between two different dimensions is not implemented yet — the "
                  "source/target parameters are in place, the lifting and projecting maths is not");
    static_assert(Flags == tg::impl::transform_canonical(Flags),
                  "transform_flags must be canonical — name the class through one of the tg aliases "
                  "(tg::rigid_transform3f, "
                  "tg::affine_transform<D, T>, ...) or use tg::transform_for<DSource, DTarget, T, Flags>, which "
                  "canonicalizes for you");

    using scalar_t = T;
    static constexpr int source_dimension = DSource;
    static constexpr int target_dimension = DTarget;

    static constexpr tg::impl::transform_flags flags = Flags;
    static constexpr tg::impl::transform_flags linear_flags = tg::impl::linear_part(Flags);

    static constexpr bool has_translation = tg::impl::has_any(Flags & tg::impl::transform_flags::translation);
    static constexpr bool has_projection = tg::impl::has_any(Flags & tg::impl::transform_flags::projection);
    /// false means every scale factor is positive, so the map preserves orientation and ordering.
    static constexpr bool has_negative_scaling = tg::impl::has_any(Flags & tg::impl::transform_flags::negative_scaling);

private:
    /// layout depends on Flags and is not API — reach for the accessors, or tg::impl::transform_representation_of.
    /// One dimension is enough while the type is square; the mixed case will want it to carry both.
    tg::impl::transform_representation<DSource, T, linear_flags, tg::impl::layout_of(Flags)> _representation;

    /// a wider or narrower class of the same family reads the members directly — that is what the widening
    /// constructor and composed() are doing, and neither can go through the public accessors without a conversion.
    template <int, int, class, tg::impl::transform_flags>
    friend struct homogeneous_transform;

    /// the return type stays deduced on both sides — see transform_representation_access.hh.
    template <int DS, int DT, class U, tg::impl::transform_flags F>
    friend constexpr auto const& tg::impl::transform_representation_of(homogeneous_transform<DS, DT, U, F> const&);

    // construction
public:
    homogeneous_transform() = default;

    /// Widening: every transform of class FS is also one of class Flags, so this never loses anything.
    ///
    /// This is also the whole dispatch mechanism.
    /// A geometric object asks, in its own order of preference, which class it can view the transform as,
    /// and the first one that compiles decides both the maths and the result type:
    ///
    ///     if constexpr (requires { tg::similarity_transform<D, T>(t); })   // a sphere stays a sphere
    ///     else if constexpr (requires { tg::affine_transform<D, T>(t); })  // ... otherwise an ellipsoid
    ///
    /// Explicit on purpose, and not merely to follow the all-constructors-are-explicit rule: an
    /// implicit widening would make a narrower transform convert to any wider one silently, which is
    /// exactly the distinction those branches rest on.
    ///
    /// A same-class conversion goes through the copy constructor, so the requires-clause excludes it
    /// and `similarity_transform<D, T>(t)` works whether t is narrower or already a similarity.
    template <tg::impl::transform_flags FS>
        requires(FS != Flags && tg::impl::transform_is_subclass(FS, Flags))
    explicit constexpr homogeneous_transform(homogeneous_transform<DSource, DTarget, T, FS> const& src)
    {
        constexpr auto src_linear = tg::impl::linear_part(FS);

        if constexpr (has_projection)
        {
            _representation.m = src.to_mat();
        }
        else
        {
            if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            {
                _representation.linear.linear = src.linear_mat();
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::scaled_rotation)
            {
                if constexpr (src_linear == tg::impl::linear_kind::scaled_rotation)
                    _representation.linear = src._representation.linear;
                else if constexpr (src_linear == tg::impl::linear_kind::rotation)
                    _representation.linear.rot = src._representation.linear.rot;
                else if constexpr (src_linear == tg::impl::linear_kind::uniform_scaling)
                    _representation.linear.scale = src._representation.linear.scale;
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::rotation)
            {
                if constexpr (src_linear == tg::impl::linear_kind::rotation)
                    _representation.linear = src._representation.linear;
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
            {
                if constexpr (src_linear == tg::impl::linear_kind::scaling)
                    _representation.linear = src._representation.linear;
                else if constexpr (src_linear == tg::impl::linear_kind::uniform_scaling)
                    _representation.linear.scale = vec<DSource, T>(src._representation.linear.scale);
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::uniform_scaling)
            {
                if constexpr (src_linear == tg::impl::linear_kind::uniform_scaling)
                    _representation.linear = src._representation.linear;
            }

            if constexpr (has_translation && tg::impl::has_any(FS & tg::impl::transform_flags::translation))
                _representation.translation = src._representation.translation;
        }
    }

    // factories
public:
    /// the displacement lives in the target space.
    [[nodiscard]] static constexpr homogeneous_transform make_translation(vec<DTarget, T> const& t)
        requires(tg::impl::transform_is_subclass(tg::impl::transform_class::translation, Flags))
    {
        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, tg::impl::transform_class::translation>::make_translation(t));
        else
        {
            homogeneous_transform r;
            r._representation.translation = t;
            return r;
        }
    }

    /// The factor must be positive unless the class carries negative_scaling.
    [[nodiscard]] static constexpr homogeneous_transform make_uniform_scaling(T scale)
        requires(tg::impl::transform_is_subclass(tg::impl::transform_class::uniform_scaling, Flags))
    {
        CC_ASSERT(has_negative_scaling || scale > T(0), "this transform class only allows positive scale factors");

        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, tg::impl::transform_class::uniform_scaling>::make_uniform_scaling(
                    scale));
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
        {
            homogeneous_transform r;
            r._representation.linear.linear = tg::impl::make_identity<DSource, DTarget, T>() * scale;
            return r;
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
        {
            homogeneous_transform r;
            r._representation.linear.scale = vec<DSource, T>(scale);
            return r;
        }
        else
        {
            homogeneous_transform r;
            r._representation.linear.scale = scale;
            return r;
        }
    }

    /// Every factor must be positive unless the class carries negative_scaling.
    [[nodiscard]] static constexpr homogeneous_transform make_scaling(vec<DSource, T> const& scale)
        requires(tg::impl::transform_is_subclass(tg::impl::transform_class::scaling, Flags))
    {
        if constexpr (!has_negative_scaling)
            for (int i = 0; i < DSource; ++i)
                CC_ASSERT(scale.data[i] > T(0), "this transform class only allows positive scale factors");

        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, tg::impl::transform_class::scaling>::make_scaling(scale));
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
        {
            homogeneous_transform r;
            for (int i = 0; i < DSource; ++i)
                r._representation.linear.linear[i, i] = scale.data[i];
            return r;
        }
        else
        {
            homogeneous_transform r;
            r._representation.linear.scale = scale;
            return r;
        }
    }

    /// rotation by `a` in the plane, counter-clockwise.
    [[nodiscard]] static homogeneous_transform make_rotation(angle<T> a)
        requires(DSource == 2 && tg::impl::transform_is_subclass(tg::impl::transform_class::rotation, Flags)
                 && tg::traits::has_trigonometry<T>)
    {
        return homogeneous_transform::make_rotation_impl(tg::impl::rotation_representation<2, T>::make_rotation(a));
    }

    /// rotation by the given unit quaternion.
    [[nodiscard]] static constexpr homogeneous_transform make_rotation(quat<T> const& q)
        requires(DSource == 3 && tg::impl::transform_is_subclass(tg::impl::transform_class::rotation, Flags))
    {
        return homogeneous_transform::make_rotation_impl(tg::impl::rotation_representation<3, T>::make_rotation(q));
    }

    /// an arbitrary linear map, as a mat with one column per SOURCE axis and one row per TARGET axis.
    /// A singular one is allowed — inverse() then yields the zero matrix.
    [[nodiscard]] static constexpr homogeneous_transform make_from_linear_mat(mat<DSource, DTarget, T> const& m)
        requires(tg::impl::transform_is_subclass(tg::impl::transform_class::linear, Flags))
    {
        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, tg::impl::transform_class::linear>::make_from_linear_mat(m));
        else
        {
            homogeneous_transform r;
            r._representation.linear.linear = m;
            return r;
        }
    }

    /// an arbitrary homogeneous matrix.
    /// Only a projective transform can hold one unchanged.
    [[nodiscard]] static constexpr homogeneous_transform make_from_mat(mat<DSource + 1, DTarget + 1, T> const& m)
        requires(has_projection)
    {
        homogeneous_transform r;
        r._representation.m = m;
        return r;
    }

    // special values
public:
    /// the identity transform.
    /// Runtime constant; a default-constructed transform is the same value.
    static homogeneous_transform const identity;

    // access
public:
    [[nodiscard]] constexpr vec<DTarget, T> translation() const
        requires(has_translation && !has_projection)
    {
        return _representation.translation;
    }

    [[nodiscard]] constexpr T uniform_scale() const
        requires(linear_flags == tg::impl::linear_kind::uniform_scaling
                 || linear_flags == tg::impl::linear_kind::scaled_rotation)
    {
        return _representation.linear.scale;
    }

    [[nodiscard]] constexpr vec<DSource, T> scale() const
        requires(linear_flags == tg::impl::linear_kind::scaling)
    {
        return _representation.linear.scale;
    }

    /// the rotation: an angle in 2D, a unit quaternion in 3D.
    /// Only available while the rotation is stored separately — a general linear part mixes it with the scaling.
    [[nodiscard]] constexpr auto rotation() const
        requires((linear_flags == tg::impl::linear_kind::rotation || linear_flags == tg::impl::linear_kind::scaled_rotation)
                 && (DSource == 3 || tg::traits::has_trigonometry<T>))
    {
        if constexpr (DSource == 2)
            return _representation.linear.rot.to_angle();
        else
            return _representation.linear.rot.to_quat();
    }

    /// the linear part as a matrix, one column per SOURCE axis and one row per TARGET axis.
    /// A projective transform has none — use to_mat().
    [[nodiscard]] constexpr mat<DSource, DTarget, T> linear_mat() const
        requires(!has_projection)
    {
        if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            return _representation.linear.linear;
        else if constexpr (linear_flags == tg::impl::linear_kind::scaled_rotation)
            return _representation.linear.rot.to_rotation_matrix() * _representation.linear.scale;
        else if constexpr (linear_flags == tg::impl::linear_kind::rotation)
            return _representation.linear.rot.to_rotation_matrix();
        else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
        {
            mat<DSource, DTarget, T> m;
            for (int i = 0; i < DSource; ++i)
                m[i, i] = _representation.linear.scale.data[i];
            return m;
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::uniform_scaling)
            return tg::impl::make_identity<DSource, DTarget, T>() * _representation.linear.scale;
        else
            return tg::impl::make_identity<DSource, DTarget, T>();
    }

    /// the homogeneous matrix, with the translation in the last column (mat is column-major).
    [[nodiscard]] constexpr mat<DSource + 1, DTarget + 1, T> to_mat() const
    {
        if constexpr (has_projection)
            return _representation.m;
        else
        {
            auto m = tg::impl::make_identity<DSource + 1, DTarget + 1, T>();
            auto const l = this->linear_mat();
            for (int c = 0; c < DSource; ++c)
                for (int r = 0; r < DTarget; ++r)
                    m[c, r] = l[c, r];

            if constexpr (has_translation)
                for (int r = 0; r < DTarget; ++r)
                    m[DSource, r] = _representation.translation.data[r];

            return m;
        }
    }

    // composition
public:
    /// the transform that applies `b` first, then this one.
    ///
    /// This is the identity that ties the three spellings together:
    ///
    ///     a(b(obj))  ==  a.composed(b).transform(obj)  ==  obj.transformed(b).transformed(a)
    ///
    /// The result class is the join of the two, which is what makes composition total:
    /// composing a rotation with a non-uniform scaling yields a general linear map, because R1 S1 R2 S2 is not of the form R S.
    /// Composing a translation with a linear map likewise yields an affine one.
    ///
    /// The argument order reads right to left, like function application and like tg::quat's a * b, which also applies b first.
    /// There is deliberately no operator* on transforms — a transform is applied, not multiplied, and `*`
    /// invites a transform * point that does not exist.
    ///
    /// The dimensions chain: `b` must land in this transform's SOURCE space, and the result runs from b's source
    /// space to this transform's target space.
    ///
    /// Composition is OPT-IN, and this is the opt-in: a transform declares `composed` for exactly the
    /// transforms it is able to absorb, and this one absorbs every class of the same scalar that chains onto it.
    /// A pair with no `composed` is not an error — tg::compose falls back to a tg::composed_transform holding both.
    /// Probe it with `requires { a.composed(b); }`.
    template <int DB, tg::impl::transform_flags FB>
    [[nodiscard]] constexpr transform_for<DB, DTarget, T, Flags | FB> composed(
        homogeneous_transform<DB, DSource, T, FB> const& b) const
    {
        using result_t = transform_for<DB, DTarget, T, Flags | FB>;
        constexpr auto lf = result_t::linear_flags;

        // both operands are first viewed at the join class, so what follows is one composition per representation kind
        // rather than one per pair of classes
        auto const wide_a = [&]
        {
            if constexpr (result_t::flags == Flags)
                return *this;
            else
                return result_t(*this);
        }();
        auto const wide_b = [&]
        {
            if constexpr (result_t::flags == FB)
                return b;
            else
                return result_t(b);
        }();

        result_t result;

        if constexpr (result_t::has_projection)
        {
            result._representation.m = wide_a._representation.m * wide_b._representation.m;
            return result;
        }
        else
        {
            if constexpr (lf == tg::impl::linear_kind::general_linear)
                result._representation.linear.linear
                    = wide_a._representation.linear.linear * wide_b._representation.linear.linear;
            else if constexpr (lf == tg::impl::linear_kind::scaled_rotation)
            {
                // the scalars commute with the rotations, so the two parts stay separate
                result._representation.linear.rot
                    = wide_a._representation.linear.rot.compose(wide_b._representation.linear.rot);
                result._representation.linear.scale
                    = wide_a._representation.linear.scale * wide_b._representation.linear.scale;
            }
            else if constexpr (lf == tg::impl::linear_kind::rotation)
                result._representation.linear.rot
                    = wide_a._representation.linear.rot.compose(wide_b._representation.linear.rot);
            else if constexpr (lf == tg::impl::linear_kind::scaling)
            {
                for (int i = 0; i < DB; ++i)
                    result._representation.linear.scale.data[i]
                        = wide_a._representation.linear.scale.data[i] * wide_b._representation.linear.scale.data[i];
            }
            else if constexpr (lf == tg::impl::linear_kind::uniform_scaling)
                result._representation.linear.scale
                    = wide_a._representation.linear.scale * wide_b._representation.linear.scale;

            if constexpr (result_t::has_translation)
                result._representation.translation
                    = wide_a.transform(wide_b._representation.translation) + wide_a._representation.translation;

            return result;
        }
    }

    /// the inverse transform, which runs from the target space back to the source space.
    ///
    /// Every canonical transform class is closed under inversion, so the class is unchanged: a rotation
    /// inverts to a rotation, a signed uniform scale to a signed uniform scale, a rigid map to a rigid map.
    ///
    /// No scale factor may be zero.
    /// A singular linear or homogeneous matrix is tolerated instead: it inverts to the zero matrix, following mat::inverse.
    [[nodiscard]] constexpr homogeneous_transform<DTarget, DSource, T, Flags> inverse() const
    {
        using transform_t = homogeneous_transform<DTarget, DSource, T, Flags>;
        constexpr auto lf = transform_t::linear_flags;

        transform_t result;

        if constexpr (transform_t::has_projection)
        {
            result._representation.m = _representation.m.inverse();
            return result;
        }
        else
        {
            if constexpr (lf == tg::impl::linear_kind::general_linear)
                result._representation.linear.linear = _representation.linear.linear.inverse();
            else if constexpr (lf == tg::impl::linear_kind::scaled_rotation)
            {
                CC_ASSERT(!tg::traits::is_zero(_representation.linear.scale), "cannot invert a transform with a zero "
                                                                              "scale");
                result._representation.linear.rot = _representation.linear.rot.inverse();
                result._representation.linear.scale = tg::one<T>() / _representation.linear.scale;
            }
            else if constexpr (lf == tg::impl::linear_kind::rotation)
                result._representation.linear.rot = _representation.linear.rot.inverse();
            else if constexpr (lf == tg::impl::linear_kind::scaling)
            {
                for (int i = 0; i < DTarget; ++i)
                {
                    CC_ASSERT(!tg::traits::is_zero(_representation.linear.scale.data[i]),
                              "cannot invert a transform with "
                              "a zero scale");
                    result._representation.linear.scale.data[i] = tg::one<T>() / _representation.linear.scale.data[i];
                }
            }
            else if constexpr (lf == tg::impl::linear_kind::uniform_scaling)
            {
                CC_ASSERT(!tg::traits::is_zero(_representation.linear.scale), "cannot invert a transform with a zero "
                                                                              "scale");
                result._representation.linear.scale = tg::one<T>() / _representation.linear.scale;
            }

            if constexpr (transform_t::has_translation)
                result._representation.translation = -result.transform(_representation.translation);

            return result;
        }
    }

    // application
public:
    /// the image of a displacement — the translation does not apply to it.
    ///
    /// This is where vec::transformed ends up, and it is the reason the linear part is applied here rather than
    /// through linear_mat(): only the transform knows whether that part is a quaternion, a scalar or a matrix.
    [[nodiscard]] constexpr vec<DTarget, T> transform(vec<DSource, T> const& v) const
    {
        if constexpr (has_projection)
        {
            static_assert(false,
                          "tg: a projective transform cannot be applied to a displacement. A free vector has no "
                          "base point, so it has no projective image — transform the endpoints and subtract instead.");
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            return _representation.linear.linear * v;
        else if constexpr (linear_flags == tg::impl::linear_kind::scaled_rotation)
            return _representation.linear.rot.apply(v) * _representation.linear.scale;
        else if constexpr (linear_flags == tg::impl::linear_kind::rotation)
            return _representation.linear.rot.apply(v);
        else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
        {
            auto r = v;
            for (int i = 0; i < DSource; ++i)
                r.data[i] *= _representation.linear.scale.data[i];
            return r;
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::uniform_scaling)
            return v * _representation.linear.scale;
        else
            return v;
    }

    /// the image of a point: the linear part, the translation, and the perspective divide if there is one.
    ///
    /// Under a projective transform the point must not map to infinity.
    /// A point behind the projection is NOT rejected — it maps to its mirror image, which is why a primitive
    /// that has to stay convex is responsible for checking its own vertices.
    [[nodiscard]] constexpr pos<DTarget, T> transform(pos<DSource, T> const& p) const
    {
        if constexpr (has_projection)
        {
            auto const& m = _representation.m;

            T w = m[DSource, DTarget];
            for (int c = 0; c < DSource; ++c)
                w += m[c, DTarget] * p.data[c];
            CC_ASSERT(!tg::traits::is_zero(w), "projective transform maps this point to infinity");

            pos<DTarget, T> result;
            for (int r = 0; r < DTarget; ++r)
            {
                T v = m[DSource, r];
                for (int c = 0; c < DSource; ++c)
                    v += m[c, r] * p.data[c];
                result.data[r] = v / w;
            }
            return result;
        }
        else
        {
            // the coordinates are copied rather than read through `p - pos()` / written through
            // `pos() + v`: that second one is not a foldable identity for floats (+0.0 + -0.0 is
            // +0.0), so it would survive into the codegen as a real xorps/addps pair
            vec<DSource, T> v;
            for (int i = 0; i < DSource; ++i)
                v.data[i] = p.data[i];

            auto image = this->transform(v);
            if constexpr (has_translation)
                image += _representation.translation;

            pos<DTarget, T> result;
            for (int i = 0; i < DTarget; ++i)
                result.data[i] = image.data[i];
            return result;
        }
    }

    /// the image of a bivector, which is NOT the linear part applied to its components.
    ///
    /// A bivector transforms by the linear map's second exterior power.
    /// In 3D its dual — a normal — therefore picks up the cofactor matrix, which is why a normal must be a bivec and not a vec.
    /// Under a rigid or similarity transform this collapses back to the rotation, up to a positive factor.
    ///
    /// The result is not renormalized; a non-uniform scaling changes its magnitude.
    [[nodiscard]] constexpr bivec<DTarget, T> transform(bivec<DSource, T> const& b) const
    {
        if constexpr (has_projection)
        {
            static_assert(false,
                          "tg: a projective transform cannot be applied to a bivector — it has no linear part in "
                          "this sense.");
        }
        // an area element is blind to the translation, as is the identity
        else if constexpr (linear_flags == tg::impl::linear_kind::identity)
            return b;
        else if constexpr (DSource == 2)
        {
            // the only component is the pseudoscalar, which scales by the determinant
            return b * this->linear_mat().determinant();
        }
        else
        {
            return tg::undual(this->linear_mat().cofactor() * tg::dual(b));
        }
    }

    /// the image of `obj` under this transform — the mirror spelling of obj.transformed(*this).
    ///
    /// vec, pos and bivec are answered by the overloads above; everything else routes straight back to the
    /// object, so there is only ever one implementation and one answer: the object's chain decides both the
    /// maths and the result type.
    ///
    /// A transform that wants to special-case a particular object does NOT do it here.
    /// It declares a PRIVATE `custom_transform(ObjT const&)` and befriends that object, which is the
    /// first thing every object's chain asks for — so the special case stays out of the public API
    /// and only the objects it was written for can reach it.
    template <class ObjT>
    [[nodiscard]] constexpr auto transform(ObjT const& obj) const
    {
        return obj.transformed(*this);
    }

    /// the call spelling of transform(obj), so a nested application reads like one:
    ///
    ///     a(b(obj))  ==  obj.transformed(b).transformed(a)
    ///
    /// Application only — `a(b)` for a transform `b` is deliberately not composition, which is spelled a.composed(b).
    template <class ObjT>
    [[nodiscard]] constexpr auto operator()(ObjT const& obj) const
    {
        return this->transform(obj);
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(homogeneous_transform const&, homogeneous_transform const&) = default;

private:
    [[nodiscard]] static constexpr homogeneous_transform make_rotation_impl(
        tg::impl::rotation_representation<DSource, T> const& rot)
    {
        homogeneous_transform result;
        if constexpr (has_projection)
        {
            auto const l = rot.to_rotation_matrix();
            for (int c = 0; c < DSource; ++c)
                for (int r = 0; r < DTarget; ++r)
                    result._representation.m[c, r] = l[c, r];
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            result._representation.linear.linear = rot.to_rotation_matrix();
        else
            result._representation.linear.rot = rot;

        return result;
    }
};

template <int DSource, int DTarget, class T, tg::impl::transform_flags Flags>
inline homogeneous_transform<DSource, DTarget, T, Flags> const homogeneous_transform<DSource, DTarget, T, Flags>::identity
    = homogeneous_transform<DSource, DTarget, T, Flags>{};

} // namespace tg
