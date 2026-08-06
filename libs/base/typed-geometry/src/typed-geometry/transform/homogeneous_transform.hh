#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/linalg/bivec.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/mat_ops.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/transform/fwd.hh>
#include <typed-geometry/transform/impl/transform_storage.hh>

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
/// The class picks the storage — a translation is a vec, a rotation a quat, a general linear part a mat — and the layout is not part of the API.
/// Read a transform back through translation(), rotation(), scale(), linear_mat() or to_mat() rather than through the storage member.
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
/// Flags must be canonical — spell a class as one of the tg::transform_class constants, or use one
/// of the aliases (tg::rigid_transform3f, tg::affine_transform3f, ...).
///
/// Equality is representational, not geometric: a quaternion and its negation denote the same
/// rotation but do not compare equal, exactly as for tg::quat.
template <int DSource, int DTarget, class T, transform_flags Flags>
struct homogeneous_transform
{
    static_assert(DSource == 2 || DSource == 3,
                  "typed-geometry transforms are 2D or 3D; the 4x4 case is the 3D homogeneous matrix");
    static_assert(DTarget == 2 || DTarget == 3,
                  "typed-geometry transforms are 2D or 3D; the 4x4 case is the 3D homogeneous matrix");
    static_assert(DSource == DTarget,
                  "a transform between two different dimensions is not implemented yet — the "
                  "source/target parameters are in place, the lifting and projecting maths is not");
    static_assert(Flags == tg::canonical(Flags),
                  "transform_flags must be canonical — name the class through tg::transform_class (rigid, similarity, "
                  "affine, ...) or use tg::transform_for<DSource, DTarget, T, Flags>, which canonicalizes for you");

    using scalar_t = T;
    static constexpr int source_dimension = DSource;
    static constexpr int target_dimension = DTarget;

    static constexpr transform_flags flags = Flags;
    static constexpr transform_flags linear_flags = tg::impl::linear_part(Flags);

    static constexpr bool has_translation = tg::has_any(Flags & transform_flags::translation);
    static constexpr bool has_projection = tg::has_any(Flags & transform_flags::projection);
    /// false means every scale factor is positive, so the map preserves orientation and ordering.
    static constexpr bool has_negative_scaling = tg::has_any(Flags & transform_flags::negative_scaling);

    /// layout depends on Flags and is not API — reach for the accessors instead.
    /// One dimension is enough while the type is square; the mixed case will want the storage to carry both.
    tg::impl::transform_storage<DSource, T, linear_flags, tg::impl::layout_of(Flags)> storage;

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
    template <transform_flags FS>
        requires(FS != Flags && tg::is_subclass(FS, Flags))
    explicit constexpr homogeneous_transform(homogeneous_transform<DSource, DTarget, T, FS> const& src)
    {
        constexpr auto src_linear = tg::impl::linear_part(FS);

        if constexpr (has_projection)
        {
            storage.m = src.to_mat();
        }
        else
        {
            if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            {
                storage.linear.linear = src.linear_mat();
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::scaled_rotation)
            {
                if constexpr (src_linear == tg::impl::linear_kind::scaled_rotation)
                    storage.linear = src.storage.linear;
                else if constexpr (src_linear == tg::impl::linear_kind::rotation)
                    storage.linear.rot = src.storage.linear.rot;
                else if constexpr (src_linear == tg::impl::linear_kind::uniform_scaling)
                    storage.linear.scale = src.storage.linear.scale;
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::rotation)
            {
                if constexpr (src_linear == tg::impl::linear_kind::rotation)
                    storage.linear = src.storage.linear;
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
            {
                if constexpr (src_linear == tg::impl::linear_kind::scaling)
                    storage.linear = src.storage.linear;
                else if constexpr (src_linear == tg::impl::linear_kind::uniform_scaling)
                    storage.linear.scale = vec<DSource, T>(src.storage.linear.scale);
            }
            else if constexpr (linear_flags == tg::impl::linear_kind::uniform_scaling)
            {
                if constexpr (src_linear == tg::impl::linear_kind::uniform_scaling)
                    storage.linear = src.storage.linear;
            }

            if constexpr (has_translation && tg::has_any(FS & transform_flags::translation))
                storage.translation = src.storage.translation;
        }
    }

    // factories
public:
    /// the displacement lives in the target space.
    [[nodiscard]] static constexpr homogeneous_transform make_translation(vec<DTarget, T> const& t)
        requires(tg::is_subclass(transform_class::translation, Flags))
    {
        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, transform_class::translation>::make_translation(t));
        else
        {
            homogeneous_transform r;
            r.storage.translation = t;
            return r;
        }
    }

    /// The factor must be positive unless the class carries negative_scaling.
    [[nodiscard]] static constexpr homogeneous_transform make_uniform_scaling(T scale)
        requires(tg::is_subclass(transform_class::uniform_scaling, Flags))
    {
        CC_ASSERT(has_negative_scaling || scale > T(0), "this transform class only allows positive scale factors");

        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, transform_class::uniform_scaling>::make_uniform_scaling(scale));
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
        {
            homogeneous_transform r;
            r.storage.linear.linear = tg::impl::make_identity<DSource, DTarget, T>() * scale;
            return r;
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
        {
            homogeneous_transform r;
            r.storage.linear.scale = vec<DSource, T>(scale);
            return r;
        }
        else
        {
            homogeneous_transform r;
            r.storage.linear.scale = scale;
            return r;
        }
    }

    /// Every factor must be positive unless the class carries negative_scaling.
    [[nodiscard]] static constexpr homogeneous_transform make_scaling(vec<DSource, T> const& scale)
        requires(tg::is_subclass(transform_class::scaling, Flags))
    {
        if constexpr (!has_negative_scaling)
            for (int i = 0; i < DSource; ++i)
                CC_ASSERT(scale.data[i] > T(0), "this transform class only allows positive scale factors");

        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, transform_class::scaling>::make_scaling(scale));
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
        {
            homogeneous_transform r;
            for (int i = 0; i < DSource; ++i)
                r.storage.linear.linear[i, i] = scale.data[i];
            return r;
        }
        else
        {
            homogeneous_transform r;
            r.storage.linear.scale = scale;
            return r;
        }
    }

    /// rotation by `a` in the plane, counter-clockwise.
    [[nodiscard]] static homogeneous_transform make_rotation(angle<T> a)
        requires(DSource == 2 && tg::is_subclass(transform_class::rotation, Flags) && tg::traits::has_trigonometry<T>)
    {
        return homogeneous_transform::make_rotation_impl(tg::impl::rotation_storage<2, T>::make_rotation(a));
    }

    /// rotation by the given unit quaternion.
    [[nodiscard]] static constexpr homogeneous_transform make_rotation(quat<T> const& q)
        requires(DSource == 3 && tg::is_subclass(transform_class::rotation, Flags))
    {
        return homogeneous_transform::make_rotation_impl(tg::impl::rotation_storage<3, T>::make_rotation(q));
    }

    /// an arbitrary linear map, as a mat with one column per SOURCE axis and one row per TARGET axis.
    /// A singular one is allowed — inverse() then yields the zero matrix.
    [[nodiscard]] static constexpr homogeneous_transform make_from_linear_mat(mat<DSource, DTarget, T> const& m)
        requires(tg::is_subclass(transform_class::linear, Flags))
    {
        if constexpr (has_projection)
            return homogeneous_transform(
                homogeneous_transform<DSource, DTarget, T, transform_class::linear>::make_from_linear_mat(m));
        else
        {
            homogeneous_transform r;
            r.storage.linear.linear = m;
            return r;
        }
    }

    /// an arbitrary homogeneous matrix.
    /// Only a projective transform can hold one unchanged.
    [[nodiscard]] static constexpr homogeneous_transform make_from_mat(mat<DSource + 1, DTarget + 1, T> const& m)
        requires(has_projection)
    {
        homogeneous_transform r;
        r.storage.m = m;
        return r;
    }

    // views at a wider class
public:
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
        return storage.translation;
    }

    [[nodiscard]] constexpr T uniform_scale() const
        requires(linear_flags == tg::impl::linear_kind::uniform_scaling
                 || linear_flags == tg::impl::linear_kind::scaled_rotation)
    {
        return storage.linear.scale;
    }

    [[nodiscard]] constexpr vec<DSource, T> scale() const
        requires(linear_flags == tg::impl::linear_kind::scaling)
    {
        return storage.linear.scale;
    }

    /// the rotation: an angle in 2D, a unit quaternion in 3D.
    /// Only available while the rotation is stored separately — a general linear part mixes it with the scaling.
    [[nodiscard]] constexpr auto rotation() const
        requires((linear_flags == tg::impl::linear_kind::rotation || linear_flags == tg::impl::linear_kind::scaled_rotation)
                 && (DSource == 3 || tg::traits::has_trigonometry<T>))
    {
        if constexpr (DSource == 2)
            return storage.linear.rot.to_angle();
        else
            return storage.linear.rot.to_quat();
    }

    /// the linear part as a matrix, one column per SOURCE axis and one row per TARGET axis.
    /// A projective transform has none — use to_mat().
    [[nodiscard]] constexpr mat<DSource, DTarget, T> linear_mat() const
        requires(!has_projection)
    {
        if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            return storage.linear.linear;
        else if constexpr (linear_flags == tg::impl::linear_kind::scaled_rotation)
            return storage.linear.rot.to_mat() * storage.linear.scale;
        else if constexpr (linear_flags == tg::impl::linear_kind::rotation)
            return storage.linear.rot.to_mat();
        else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
        {
            mat<DSource, DTarget, T> m;
            for (int i = 0; i < DSource; ++i)
                m[i, i] = storage.linear.scale.data[i];
            return m;
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::uniform_scaling)
            return tg::impl::make_identity<DSource, DTarget, T>() * storage.linear.scale;
        else
            return tg::impl::make_identity<DSource, DTarget, T>();
    }

    /// the homogeneous matrix, with the translation in the last column (mat is column-major).
    [[nodiscard]] constexpr mat<DSource + 1, DTarget + 1, T> to_mat() const
    {
        if constexpr (has_projection)
            return storage.m;
        else
        {
            auto m = tg::impl::make_identity<DSource + 1, DTarget + 1, T>();
            auto const l = this->linear_mat();
            for (int c = 0; c < DSource; ++c)
                for (int r = 0; r < DTarget; ++r)
                    m[c, r] = l[c, r];

            if constexpr (has_translation)
                for (int r = 0; r < DTarget; ++r)
                    m[DSource, r] = storage.translation.data[r];

            return m;
        }
    }

    /// the homogeneous weight this point picks up.
    ///
    /// A finite convex primitive survives a projective map exactly when this is positive at every
    /// vertex: w is affine over the primitive and the positive-w halfspace is convex, so checking
    /// the vertices settles the whole hull.
    [[nodiscard]] constexpr T homogeneous_w(pos<DSource, T> const& p) const
        requires(has_projection)
    {
        T w = storage.m[DSource, DTarget];
        for (int c = 0; c < DSource; ++c)
            w += storage.m[c, DTarget] * p.data[c];
        return w;
    }

    /// apply only the linear part, i.e. transform a displacement rather than a point.
    ///
    /// A projective transform has no linear part in this sense — a free vector has no projective image.
    [[nodiscard]] constexpr vec<DTarget, T> apply_linear(vec<DSource, T> const& v) const
        requires(!has_projection)
    {
        if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            return storage.linear.linear * v;
        else if constexpr (linear_flags == tg::impl::linear_kind::scaled_rotation)
            return storage.linear.rot.apply(v) * storage.linear.scale;
        else if constexpr (linear_flags == tg::impl::linear_kind::rotation)
            return storage.linear.rot.apply(v);
        else if constexpr (linear_flags == tg::impl::linear_kind::scaling)
        {
            auto r = v;
            for (int i = 0; i < DSource; ++i)
                r.data[i] *= storage.linear.scale.data[i];
            return r;
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::uniform_scaling)
            return v * storage.linear.scale;
        else
            return v;
    }

    /// the image of a point: the linear part, the translation, and the perspective divide if there is one.
    ///
    /// Under a projective transform the point must not map to infinity.
    [[nodiscard]] constexpr pos<DTarget, T> apply_pos(pos<DSource, T> const& p) const
    {
        if constexpr (has_projection)
        {
            auto const& m = storage.m;

            T const w = this->homogeneous_w(p);
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

            auto image = this->apply_linear(v);
            if constexpr (has_translation)
                image += storage.translation;

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
    [[nodiscard]] constexpr bivec<DTarget, T> apply_bivec(bivec<DSource, T> const& b) const
        requires(!has_projection)
    {
        if constexpr (DSource == 2)
        {
            // the only component is the pseudoscalar, which scales by the determinant
            return b * tg::determinant(this->linear_mat());
        }
        else
        {
            return tg::undual(tg::cofactor(this->linear_mat()) * tg::dual(b));
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
    template <int DB, transform_flags FB>
    [[nodiscard]] constexpr transform_for<DB, DTarget, T, Flags | FB> composed(
        homogeneous_transform<DB, DSource, T, FB> const& b) const
    {
        using result_t = transform_for<DB, DTarget, T, Flags | FB>;
        constexpr auto lf = result_t::linear_flags;

        // both operands are first viewed at the join class, so what follows is one composition per storage kind
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
            result.storage.m = wide_a.storage.m * wide_b.storage.m;
            return result;
        }
        else
        {
            if constexpr (lf == tg::impl::linear_kind::general_linear)
                result.storage.linear.linear = wide_a.storage.linear.linear * wide_b.storage.linear.linear;
            else if constexpr (lf == tg::impl::linear_kind::scaled_rotation)
            {
                // the scalars commute with the rotations, so the two parts stay separate
                result.storage.linear.rot = wide_a.storage.linear.rot.compose(wide_b.storage.linear.rot);
                result.storage.linear.scale = wide_a.storage.linear.scale * wide_b.storage.linear.scale;
            }
            else if constexpr (lf == tg::impl::linear_kind::rotation)
                result.storage.linear.rot = wide_a.storage.linear.rot.compose(wide_b.storage.linear.rot);
            else if constexpr (lf == tg::impl::linear_kind::scaling)
            {
                for (int i = 0; i < DB; ++i)
                    result.storage.linear.scale.data[i]
                        = wide_a.storage.linear.scale.data[i] * wide_b.storage.linear.scale.data[i];
            }
            else if constexpr (lf == tg::impl::linear_kind::uniform_scaling)
                result.storage.linear.scale = wide_a.storage.linear.scale * wide_b.storage.linear.scale;

            if constexpr (result_t::has_translation)
                result.storage.translation = wide_a.apply_linear(wide_b.storage.translation) + wide_a.storage.translation;

            return result;
        }
    }

    // application
public:
    /// the image of `obj` under this transform — the mirror spelling of obj.transformed(*this).
    ///
    /// It routes straight back to the object, so there is only ever one implementation and one
    /// answer: the object's chain decides both the maths and the result type.
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
        return obj.transformed(*this);
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(homogeneous_transform const&, homogeneous_transform const&) = default;

private:
    [[nodiscard]] static constexpr homogeneous_transform make_rotation_impl(tg::impl::rotation_storage<DSource, T> const& rot)
    {
        homogeneous_transform result;
        if constexpr (has_projection)
        {
            auto const l = rot.to_mat();
            for (int c = 0; c < DSource; ++c)
                for (int r = 0; r < DTarget; ++r)
                    result.storage.m[c, r] = l[c, r];
        }
        else if constexpr (linear_flags == tg::impl::linear_kind::general_linear)
            result.storage.linear.linear = rot.to_mat();
        else
            result.storage.linear.rot = rot;

        return result;
    }
};

template <int DSource, int DTarget, class T, transform_flags Flags>
inline homogeneous_transform<DSource, DTarget, T, Flags> const homogeneous_transform<DSource, DTarget, T, Flags>::identity
    = homogeneous_transform<DSource, DTarget, T, Flags>{};

} // namespace tg
