#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/linalg/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

// transformed() names the transform classes to branch on them.
// This is forward declarations only — transform/fwd.hh pulls in nothing from linalg, so the
// dependency stays one-way; the complete type is needed at the call site, not here.
#include <typed-geometry/transform/fwd.hh>

#include <initializer_list>

namespace tg
{
/// Position / point in D dimensions.
///
/// A pos is a point in space — conceptually a singleton point set, not a free vector.
/// Its arithmetic with vec mirrors affine geometry:
///
///     pos - pos -> vec    (the displacement between two points)
///     pos + vec -> pos    (translate a point)
///     pos - vec -> pos    (translate a point)
///     pos + pos -> pos    (translation of the singleton point set — a deliberate rule;
///                          adding a point translates by that point's coordinates)
///
/// The raw storage is the public C array member `data`.
/// Components are accessed through `data` or operator[] — there are no .x/.y/.z members.
/// Default construction zero-initializes all components (the origin).
///
///     tg::pos3f o;                          // origin {0, 0, 0}
///     auto const p = tg::pos3f(1, 2, 3);
///     auto const q = tg::pos3f(4, 6, 3);
///     auto const d = q - p;                 // tg::vec3f{3, 4, 0}
///     auto const r = p + d;                 // back to q
template <int D, class T>
struct pos
{
    static_assert(D > 0, "pos requires a positive dimension");

    T data[D] = {};

    // construction
public:
    pos() = default;

    explicit constexpr pos(T value)
    {
        for (int i = 0; i < D; ++i)
            data[i] = value;
    }

    explicit constexpr pos(T x, T y)
        requires(D == 2)
      : data{x, y}
    {
    }
    explicit constexpr pos(T x, T y, T z)
        requires(D == 3)
      : data{x, y, z}
    {
    }
    explicit constexpr pos(T x, T y, T z, T w)
        requires(D == 4)
      : data{x, y, z, w}
    {
    }

    explicit pos(std::initializer_list<T> values)
    {
        CC_ASSERT(isize(values.size()) == D, "initializer list size must match dimension");
        int i = 0;
        for (auto const& v : values)
            data[i++] = v;
    }

    /// variadic construction; exactly D arguments are required.
    template <class... Ts>
    [[nodiscard]] static constexpr pos make_from_values(Ts... values)
        requires(sizeof...(Ts) == D)
    {
        pos r;
        int i = 0;
        ((r.data[i++] = T(values)), ...);
        return r;
    }

    // special values
public:
    /// the origin (all coordinates zero). Runtime constant, not usable in constant expressions.
    static pos const zero;

    // access
public:
    [[nodiscard]] constexpr T& operator[](int i)
    {
        CC_ASSERT(0 <= i && i < D, "component index out of range");
        return data[i];
    }
    [[nodiscard]] constexpr T const& operator[](int i) const
    {
        CC_ASSERT(0 <= i && i < D, "component index out of range");
        return data[i];
    }

    // transformation
public:
    /// the image of this point under `t`, branching on what the transform's class actually is.
    ///
    /// The narrow classes get the cheap answer directly: the identity returns the point, and a pure
    /// translation is a single addition, with no linear part touched at all.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::identity_transform<D, T>(t); })
            return *this;
        else if constexpr (requires { tg::translation_transform<D, T>(t); })
            return *this + t.translation();

        // everything else — linear, affine, projective — is applied by the transform, which is the only
        // thing that knows its storage: going through linear_mat() here would build a matrix for what may be a single quaternion.
        // Every class widens to projective, so this is the general case.
        else if constexpr (requires { tg::projective_transform<D, T>(t); })
            return t.apply_pos(*this);
        else
            static_assert(false, "tg: this transform cannot be applied to a point");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(pos const&, pos const&) = default;

    // affine arithmetic
public:
    /// displacement between two points.
    [[nodiscard]] friend constexpr vec<D, T> operator-(pos const& a, pos const& b)
    {
        vec<D, T> r;
        for (int i = 0; i < D; ++i)
            r.data[i] = a.data[i] - b.data[i];
        return r;
    }
    [[nodiscard]] friend constexpr pos operator+(pos a, vec<D, T> const& v)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += v.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr pos operator+(vec<D, T> const& v, pos a)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += v.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr pos operator-(pos a, vec<D, T> const& v)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= v.data[i];
        return a;
    }

    /// translation of the singleton point set: pos + pos -> pos (adds coordinates).
    [[nodiscard]] friend constexpr pos operator+(pos a, pos const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += b.data[i];
        return a;
    }

    friend constexpr pos& operator+=(pos& a, vec<D, T> const& v)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += v.data[i];
        return a;
    }
    friend constexpr pos& operator-=(pos& a, vec<D, T> const& v)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= v.data[i];
        return a;
    }
};

template <int D, class T>
inline pos<D, T> const pos<D, T>::zero = pos<D, T>{};

} // namespace tg
