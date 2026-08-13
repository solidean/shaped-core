#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash128.hh> // cc::hash128
#include <clean-core/common/utility.hh> // cc::forward, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

#include <type_traits>

namespace sv
{
/// The scalar an attribute element or a parameter value is built from.
/// This list is complete — every scalar sv stores; anything wider is a vector or a matrix of one of these.
enum class scalar_type : u8
{
    i8,
    i16,
    i32,
    i64,
    u8,
    u16,
    u32,
    u64,
    f32,
    f64,

    /// one byte holding 0 or 1 — its own type rather than a u8, so a consumer knows it is a flag and not a small number
    boolean,
};

/// Size of one `scalar_type`, in bytes.
[[nodiscard]] constexpr i32 scalar_type_size(scalar_type type)
{
    switch (type)
    {
    case scalar_type::i8:
    case scalar_type::u8:
    case scalar_type::boolean:
        return 1;
    case scalar_type::i16:
    case scalar_type::u16:
        return 2;
    case scalar_type::i32:
    case scalar_type::u32:
    case scalar_type::f32:
        return 4;
    case scalar_type::i64:
    case scalar_type::u64:
    case scalar_type::f64:
        return 8;
    }
    return 0;
}

} // namespace sv

/// The element layout of an attribute: a scalar type plus its dimensionality, so a consumer can interpret the bytes without a separate stride.
///
/// Dimensionality rather than one enumerator per type, because the cross product of scalars and shapes is what a format list otherwise has to spell out.
/// `dim0` and `dim1` are 1 for a scalar, `dim0` alone for a vector, and both for a column-major matrix — `dim0` the row count, `dim1` the column count, matching tg's `mat<C, R, T>`.
/// They are numbered rather than named after rows and columns because only the matrix case has rows and columns at all.
/// Both must be in 1..4.
struct sv::attribute_format
{
    sv::scalar_type scalar = scalar_type::f32;
    int dim0 = 1;
    int dim1 = 1;

    [[nodiscard]] static constexpr attribute_format of_scalar(sv::scalar_type scalar) { return {.scalar = scalar}; }

    /// A `dimension`-component vector, so `of_vector(scalar_type::f32, 3)` is a tg::vec3f.
    [[nodiscard]] static constexpr attribute_format of_vector(sv::scalar_type scalar, i32 dimension)
    {
        CC_ASSERT(dimension >= 1 && dimension <= 4, "attribute vectors are 1 to 4 components");
        return {.scalar = scalar, .dim0 = u8(dimension)};
    }

    /// A column-major matrix of `cols` columns and `rows` rows, so `of_matrix(scalar_type::f32, 3, 3)` is a tg::mat3f.
    [[nodiscard]] static constexpr attribute_format of_matrix(sv::scalar_type scalar, i32 rows, i32 cols)
    {
        CC_ASSERT(rows >= 1 && rows <= 4 && cols >= 1 && cols <= 4, "attribute matrices are 1 to 4 rows and columns");
        return {.scalar = scalar, .dim0 = u8(rows), .dim1 = u8(cols)};
    }

    [[nodiscard]] constexpr int size_bytes() const { return scalar_type_size(scalar) * dim0 * dim1; }
    [[nodiscard]] constexpr int component_count() const { return dim0 * dim1; }

    [[nodiscard]] constexpr bool is_scalar() const { return dim0 == 1 && dim1 == 1; }
    [[nodiscard]] constexpr bool is_vector() const { return dim0 > 1 && dim1 == 1; }
    [[nodiscard]] constexpr bool is_matrix() const { return dim1 > 1; }

    [[nodiscard]] friend constexpr bool operator==(attribute_format, attribute_format) = default;
};

namespace sv
{

/// How many elements an attribute carries, i.e. what its index means.
///
/// `per_vertex` indexes the geometry's position buffer, so an indexed mesh shares an element across the triangles meeting at a vertex.
/// `per_corner` is 3 elements per triangle, in triangle order — what a hard edge needs, since the two triangles then carry their own normal at the shared vertex.
/// `per_triangle` is one element per triangle, indexed by `PrimitiveIndex()`.
///
/// `per_edge` is RESERVED and rejected by `mesh_attribute::create` for now.
/// The other three index something the geometry already numbers; an edge is not numbered at all, so per-edge data needs an edge table on triangle_geometry first —
/// the edges themselves (each naming its two vertices) plus each triangle's three edge indices, which is also what decides whether opposite half-edges share one entry.
/// Only the mapping is missing: nothing else here changes when it lands, which is why the enumerator is spelled out now rather than renumbering later.
enum class attribute_frequency : u8
{
    per_vertex,
    per_corner,
    per_triangle,
    per_edge,
};

namespace impl
{
/// The scalar_type a C++ arithmetic type maps to.
/// Specialized for exactly the supported ones, so an element of any other type fails to compile rather than being reinterpreted.
template <class T>
struct scalar_type_trait;

template <>
struct scalar_type_trait<i8>
{
    static constexpr scalar_type value = scalar_type::i8;
};
template <>
struct scalar_type_trait<i16>
{
    static constexpr scalar_type value = scalar_type::i16;
};
template <>
struct scalar_type_trait<i32>
{
    static constexpr scalar_type value = scalar_type::i32;
};
template <>
struct scalar_type_trait<i64>
{
    static constexpr scalar_type value = scalar_type::i64;
};
template <>
struct scalar_type_trait<u8>
{
    static constexpr scalar_type value = scalar_type::u8;
};
template <>
struct scalar_type_trait<u16>
{
    static constexpr scalar_type value = scalar_type::u16;
};
template <>
struct scalar_type_trait<u32>
{
    static constexpr scalar_type value = scalar_type::u32;
};
template <>
struct scalar_type_trait<u64>
{
    static constexpr scalar_type value = scalar_type::u64;
};
template <>
struct scalar_type_trait<f32>
{
    static constexpr scalar_type value = scalar_type::f32;
};
template <>
struct scalar_type_trait<f64>
{
    static constexpr scalar_type value = scalar_type::f64;
};
template <>
struct scalar_type_trait<bool>
{
    static constexpr scalar_type value = scalar_type::boolean;
};

/// The attribute_format an element type maps to.
/// The primary handles the scalars; tg's containers are partial specializations, so every scalar/shape combination works without being listed.
template <class T>
struct attribute_format_trait
{
    static constexpr attribute_format value = attribute_format::of_scalar(scalar_type_trait<T>::value);
};

template <int D, class T>
struct attribute_format_trait<tg::vec<D, T>>
{
    static constexpr attribute_format value = attribute_format::of_vector(scalar_type_trait<T>::value, D);
};
template <int D, class T>
struct attribute_format_trait<tg::pos<D, T>>
{
    static constexpr attribute_format value = attribute_format::of_vector(scalar_type_trait<T>::value, D);
};
template <int D, class T>
struct attribute_format_trait<tg::comp<D, T>>
{
    static constexpr attribute_format value = attribute_format::of_vector(scalar_type_trait<T>::value, D);
};
template <int C, int R, class T>
struct attribute_format_trait<tg::mat<C, R, T>>
{
    static constexpr attribute_format value = attribute_format::of_matrix(scalar_type_trait<T>::value, R, C);
};
} // namespace impl

/// The format an element type is stored as — what `mesh_attribute::create` deduces, and what `elements_as<T>()` checks against.
template <class T>
inline constexpr attribute_format attribute_format_of = impl::attribute_format_trait<T>::value;

} // namespace sv

/// One named array of arbitrary per-element mesh data — normals, texture coordinates, colors, curvature, whatever the material asks for.
///
/// The payload is type-erased to bytes plus a `format` saying how to read them, so a mesh carries any number of attributes in one list.
/// `name` is what a material looks the attribute up by; it is the contract between the two, and a material that misses one falls back rather than failing.
/// `frequency` decides what an element index means, so it must agree with the geometry: `element_count()` must be its vertex, corner or triangle count.
///
/// The bytes are pinned and hashed like the geometry: the mesh keeps them alive, and equal contents carry equal hashes so an upload caches on the hash alone.
struct sv::mesh_attribute
{
    cc::string name;

    attribute_format format = attribute_format::of_scalar(scalar_type::f32);
    attribute_frequency frequency = attribute_frequency::per_vertex;

    cc::pinned_data<byte const> data;
    cc::hash128 hash;

    /// Pins `elements` and hashes their bytes, deducing `format` from the element type.
    /// The element type must be a scalar or a tg vector / matrix over one — whatever `attribute_format_of` names.
    /// `frequency` must not be per_edge: an edge has no index until triangle_geometry carries an edge table.
    template <class Elements>
    [[nodiscard]] static mesh_attribute create(cc::string name, attribute_frequency frequency, Elements&& elements)
    {
        CC_ASSERT(frequency != attribute_frequency::per_edge, "per_edge attributes need an edge table, which does not "
                                                              "exist yet");

        auto pinned = cc::make_pinned_data(cc::forward<Elements>(elements));
        using element_t = std::remove_const_t<std::remove_reference_t<decltype(*pinned.data())>>;

        auto bytes = pinned.as_bytes();
        auto const hash = cc::hash128::create(bytes.span(), impl::attribute_hash_seed);
        return {.name = cc::move(name),
                .format = attribute_format_of<element_t>,
                .frequency = frequency,
                .data = cc::move(bytes),
                .hash = hash};
    }

    [[nodiscard]] isize element_count() const { return data.size() / format.size_bytes(); }

    /// The elements read back as T, which must be the type `format` names.
    template <class T>
    [[nodiscard]] cc::span<T const> elements_as() const
    {
        CC_ASSERT(attribute_format_of<T> == format, "attribute holds a different element type");
        auto const view = data.span().try_reinterpret_as<T const>();
        CC_ASSERT(view.has_value(), "attribute byte count is not a multiple of the element size");
        return view.value();
    }
};
