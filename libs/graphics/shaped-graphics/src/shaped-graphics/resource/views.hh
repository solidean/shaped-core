#pragma once

#include <clean-core/common/assert.hh>     // CC_ASSERT (access checks on the raw -> typed recovery)
#include <clean-core/common/hash.hh>       // cc::make_hash (the arms' view-identity hidden friends)
#include <clean-core/common/utility.hh>    // cc::move
#include <clean-core/container/variant.hh> // cc::variant (raw_view is a sum over the per-resource payloads)
#include <clean-core/error/optional.hh>    // cc::optional (try_as_* recovery)
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh> // render_target_view / depth_stencil_view read the texture's extent
#include <shaped-graphics/resource/subresource.hh> // subresource_range (texture view sub-selection)
#include <typed-geometry/linalg/vec.hh>            // tg::vec4f (clear color builder)

#include <type_traits>

/// Strongly-typed resource views: a typed handle onto a resource, or a sub-range of one, read as a shader-facing binding.
/// A routine takes exactly the view it operates on, instead of a raw resource plus an overload set.
/// libs/graphics/shaped-graphics/docs/concepts/views.md is the concept — the two axes, the placement rules, and the erasure model.
///
/// Erasure is not one-way for buffer and texture views: each layer offers an `as_<class>()` and a `try_as_<class>()` twin back toward the typed leaves.
/// `as_*` asserts on mismatch; `try_as_*` returns nullopt instead.
/// A `try_as_*` tolerates the runtime view class being wrong, and on a texture arm the runtime `view_dimension` too.
/// A buffer's caller-supplied element `T` asserts either way, since a wrong element size is a claim the view's stride can disprove.
/// Each recovery below documents only the check it adds on top of that.

namespace sg
{
/// A view's element (`readonly` / `readwrite`) or block (`constants`) type.
/// Must be `byte`, the byte-addressed path, or a multiple of 4 bytes, since GPUs load at DWORD alignment.
template <class T>
concept view_element = std::is_same_v<T, byte> || (sizeof(T) % 4 == 0);

/// Placement rules a constants buffer view must satisfy.
/// Its byte offset must be a multiple of `constants_buffer_offset_alignment`, and its size a multiple of 16 (std140 packing) and at most `max_constants_buffer_size`.
constexpr isize constants_buffer_offset_alignment = 256; // Vk minUniformBufferOffsetAlignment / WGPU / DX12 CBV placement
constexpr isize max_constants_buffer_size = 65536;       // 64 KiB — DX12 max CBV / WGPU max uniform binding

/// Placement rules a shader-facing *storage* buffer view — readonly or readwrite, bytes or structured — must satisfy.
/// A view is a subrange, so it carries the binding-offset rules; `buffer<T>` is a whole buffer recast like a span and carries none of them.
/// Both values are portable floors, hardcoded rather than queried per device, so a violation fails on a dx12 dev box rather than later on WebGPU.
constexpr isize storage_buffer_offset_alignment
    = 256; // WGPU minStorageBufferOffsetAlignment; Vk lets an implementation require up to 256
constexpr isize storage_buffer_size_alignment = 4; // a WGPU storage binding's size must be a multiple of 4

/// A `constants_buffer_view` block type: a `view_element` whose size obeys the constants block rules above.
/// `byte` is excluded — a constants block of raw bytes is meaningless.
template <class T>
concept constants_element = view_element<T> && (sizeof(T) % 16 == 0) && (isize(sizeof(T)) <= max_constants_buffer_size);

} // namespace sg

/// How a shader reads a view.
/// A buffer view is `constants`, `readonly` or `readwrite`, mirroring `buffer_usage`; a texture view is a `texture` or an `image`.
/// An image's access — read, write or both — belongs to the binding, not the view, since every backend builds the same descriptor for all three.
enum class sg::view_class
{
    constants,              ///< a constants block — constant buffer / UBO (read-only)
    readonly,               ///< read-only storage buffer — SRV / read SSBO
    readwrite,              ///< read-write storage buffer — UAV / read-write SSBO
    texture,                ///< sampled texture — SRV / sampled image
    image,                  ///< storage image, whatever the shader's access — UAV / storage image
    acceleration_structure, ///< ray-tracing TLAS — a read-only SRV addressed by GPU VA (no bound resource)
    // Future (with a graphics pipeline / samplers): render_target, depth_stencil, sampler.
};

/// How a view's bytes are laid out.
/// `bytes` is byte-addressed (element type `byte`), `structured` an array strided by the element type, `constants_block` a single struct block.
/// `texture` is a texel grid, whose dimension / array / cube / sample count come from the bound raw_texture's description.
enum class sg::view_shape
{
    constants_block,
    structured,
    bytes,
    texture,
    acceleration_structure, ///< a ray-tracing TLAS bound as an SRV — no byte layout, addressed by the AS's GPU VA
    // Future (with formats): texel (a typed buffer view).
};

/// The shader-facing dimensionality of a texture view — how the bound texels are declared in the shader.
/// HLSL `Texture2D` / `Texture2DArray` / `TextureCube` / …; Vulkan `VkImageViewType`; D3D `SRV`/`UAV_DIMENSION`.
/// A reinterpretation the view chooses, distinct from the texture's own `texture_dimension`.
/// One slice of a 2D array is `tex_2d`, a cube face is `tex_2d`, one cube of a cube array is `cube`.
/// Image views only use the non-cube, non-multisampled members.
enum class sg::texture_view_dimension : sg::u8
{
    tex_1d,
    tex_1d_array,
    tex_2d,
    tex_2d_array,
    tex_2d_ms,       ///< multisampled 2D (sampled-only; `Load`-based)
    tex_2d_ms_array, ///< multisampled 2D array (also how a multisampled cube is sampled — no `TextureCubeMS`)
    tex_3d,
    cube,
    cube_array,
};

namespace sg
{

/// A dimension an image view may bind as: no cube, no multisampling.
/// A cube binds as an image of a 2D array, and MSAA has no image at all.
/// Declared here rather than beside `image_view` so the erased `raw_texture_view` arm can name it in its recovery accessors.
template <texture_view_dimension Dim>
concept image_view_dimension
    = Dim != texture_view_dimension::cube && Dim != texture_view_dimension::cube_array
   && Dim != texture_view_dimension::tex_2d_ms && Dim != texture_view_dimension::tex_2d_ms_array;

// The erased form every typed view converts into is `raw_view` (below) — a sum over one cohesive payload per resource kind.
// The three payload arms are also the raw binding vocabulary, for tooling that builds bindings without the typed wrappers.

// The typed views below are defined qualified, and a constrained template can only be declared where its concept is — here, rather than in fwd.hh.
template <constants_element T>
struct constants_buffer_view;
template <view_element T>
struct readonly_buffer_view;
template <view_element T>
struct readwrite_buffer_view;
template <view_element T>
struct buffer_view;
template <texture_view_dimension Dim>
struct texture_view_traits;
template <class Traits>
struct texture_view;
template <class Traits, pixel_format Format>
    requires image_view_dimension<Traits::dimension>
struct image_view;
template <class Traits>
struct any_texture_view;

} // namespace sg

/// A buffer view's erased payload: how it is bound, its byte layout and buffer, which a backend reads to build a CBV / SRV / UAV.
/// `shape` picks the interpretation — constants block, structured array, or bytes.
struct sg::raw_buffer_view
{
    view_class bound_as = view_class::readonly; ///< constants / readonly / readwrite
    view_shape shape = view_shape::structured;  ///< constants_block / structured / bytes
    raw_buffer_handle buffer;                   ///< the viewed buffer
    isize offset_in_bytes = 0;                  ///< start of the view within the buffer
    isize size_in_bytes = 0;                    ///< [constants_block, bytes] visible byte size
    isize element_count = 0;                    ///< [structured] number of elements
    isize stride_in_bytes = 0;                  ///< [structured] element stride (= sizeof(T))

    /// View-IDENTITY hash (the cc::make_hash protocol's hidden friend): the buffer by address, plus every
    /// field that reaches the descriptor — never the buffer's contents.
    [[nodiscard]] friend u64 hash(raw_buffer_view const& v)
    {
        return cc::make_hash(v.buffer.get(), v.bound_as, v.shape, v.offset_in_bytes, v.size_in_bytes, v.element_count,
                             v.stride_in_bytes);
    }

    /// View IDENTITY equality over the same fields `hash` above folds — a hash map keyed on views needs the
    /// two to agree, and defaulting it is what keeps them agreeing as fields are added.
    /// The handle compares by stored pointer, which is the `.get()` the hash takes.
    [[nodiscard]] friend bool operator==(raw_buffer_view const&, raw_buffer_view const&) = default;

    // Re-type this erased arm as a strongly-typed leaf of element `T`, which you supply — no element tag is stored, so `T` is your claim about the bytes.
    // Adds the layout check `buffer_view<T>` does, since it delegates there.
    // Member function templates, so the struct stays an aggregate and brace / designated init is unaffected.
    template <view_element T>
    [[nodiscard]] auto as_readonly() const; // -> readonly_buffer_view<T>
    template <view_element T>
    [[nodiscard]] auto as_readwrite() const; // -> readwrite_buffer_view<T>
    template <constants_element T>
    [[nodiscard]] auto as_constants() const; // -> constants_buffer_view<T>
    template <view_element T>
    [[nodiscard]] auto try_as_readonly() const; // -> cc::optional<readonly_buffer_view<T>>
    template <view_element T>
    [[nodiscard]] auto try_as_readwrite() const; // -> cc::optional<readwrite_buffer_view<T>>
    template <constants_element T>
    [[nodiscard]] auto try_as_constants() const; // -> cc::optional<constants_buffer_view<T>>
};

/// A vacant array element: no view at all, marked explicitly.
/// The backend synthesizes a null descriptor for it from the *binding* alone — view class and shape from
/// its kind and access, a texture's dimension from `binding.texture_dimension` — so it carries nothing.
/// Only valid as an element of an array binding; a scalar binding must bind a resource.
struct sg::vacant_view
{
    /// All vacancies are one value; the enclosing raw_view's hash separates the arm.
    [[nodiscard]] friend u64 hash(vacant_view const&) { return 0; }

    /// One value, so any two vacancies are equal — the counterpart of the constant hash above.
    [[nodiscard]] friend bool operator==(vacant_view const&, vacant_view const&) { return true; }
};

/// A texture view's erased payload: the texture (SRV) or image (UAV) descriptor a backend builds over a subresource range.
/// Dimension and format are a reinterpretation the view chose, not the texture's shape.
struct sg::raw_texture_view
{
    view_class bound_as = view_class::texture;                              ///< texture (SRV) / image (UAV)
    raw_texture_handle texture;                                             ///< the viewed texture
    texture_view_dimension view_dimension = texture_view_dimension::tex_2d; ///< shader-facing SRV/UAV dimension
    pixel_format format = pixel_format::undefined; ///< the format the descriptor reads/writes as
    subresource_range range;                       ///< the mip × array-slice × aspect sub-range the view exposes
    cc::start_end depth_slice_range
        = {.start = 0, .end = 0}; ///< [3D image view] depth (W/Z) slice window; empty otherwise

    /// View-IDENTITY hash (the cc::make_hash protocol's hidden friend): the texture by address, plus every
    /// field that reaches the descriptor — never the texture's texels.
    [[nodiscard]] friend u64 hash(raw_texture_view const& v)
    {
        return cc::make_hash(v.texture.get(), v.bound_as, v.view_dimension, v.format, v.range,
                             v.depth_slice_range.start, v.depth_slice_range.end);
    }

    /// View IDENTITY equality over the same fields `hash` above folds — a hash map keyed on views needs the
    /// two to agree, and defaulting it is what keeps them agreeing as fields are added.
    /// The handle compares by stored pointer, which is the `.get()` the hash takes.
    [[nodiscard]] friend bool operator==(raw_texture_view const&, raw_texture_view const&) = default;

    // Re-type this erased arm as a strongly-typed leaf of shape `Traits`, which you supply.
    // Adds a check that the runtime `view_dimension` matches `Traits::dimension`.
    template <class Traits>
    [[nodiscard]] auto as_texture() const; // -> texture_view<Traits>
    template <class Traits, pixel_format Format>
        requires image_view_dimension<Traits::dimension>
    [[nodiscard]] auto as_image() const; // -> image_view<Traits, Format>
    template <class Traits>
    [[nodiscard]] auto try_as_texture() const; // -> cc::optional<texture_view<Traits>>
    template <class Traits, pixel_format Format>
        requires image_view_dimension<Traits::dimension>
    [[nodiscard]] auto try_as_image() const; // -> cc::optional<image_view<Traits, Format>>
};

/// An acceleration-structure view's erased payload: the abstract TLAS, which each backend binds its own way.
/// It is always bound as an `acceleration_structure`.
struct sg::raw_tlas_view
{
    tlas_handle tlas; ///< the viewed top-level acceleration structure

    /// View-IDENTITY hash: the TLAS by address (a null TLAS hashes as null).
    [[nodiscard]] friend u64 hash(raw_tlas_view const& v) { return cc::make_hash(v.tlas.get()); }

    /// View IDENTITY equality over the same TLAS `hash` above folds — the handle compares by stored pointer,
    /// which is the `.get()` the hash takes.
    [[nodiscard]] friend bool operator==(raw_tlas_view const&, raw_tlas_view const&) = default;
};

namespace sg
{

/// The erased form every typed view converts into — a sum over the per-resource payloads, plus the vacant marker.
/// A backend `visit`s it, or reaches for one of the `try_as_*_view` arm accessors below, to build the native descriptor; `named_view` carries one.
using raw_view = cc::variant<raw_buffer_view, raw_texture_view, raw_tlas_view, vacant_view>;

/// Whether the erased view is the vacant marker — an array element deliberately left empty.
/// Gate on this before view_class_of / shape_of, which have no answer for a vacant element.
[[nodiscard]] inline bool is_vacant(raw_view const& v)
{
    return v.try_as<vacant_view>() != nullptr;
}

/// The buffer arm, or null when the erased view holds a different one.
[[nodiscard]] inline raw_buffer_view const* try_as_buffer_view(raw_view const& v)
{
    return v.try_as<raw_buffer_view>();
}
/// The texture arm, or null when the erased view holds a different one.
[[nodiscard]] inline raw_texture_view const* try_as_texture_view(raw_view const& v)
{
    return v.try_as<raw_texture_view>();
}
/// The tlas arm, or null when the erased view holds a different one.
[[nodiscard]] inline raw_tlas_view const* try_as_tlas_view(raw_view const& v)
{
    return v.try_as<raw_tlas_view>();
}

/// The buffer arm; the erased view must hold one.
[[nodiscard]] inline raw_buffer_view const& as_buffer_view(raw_view const& v)
{
    return v.as<raw_buffer_view>();
}
/// The texture arm; the erased view must hold one.
[[nodiscard]] inline raw_texture_view const& as_texture_view(raw_view const& v)
{
    return v.as<raw_texture_view>();
}
/// The tlas arm; the erased view must hold one.
[[nodiscard]] inline raw_tlas_view const& as_tlas_view(raw_view const& v)
{
    return v.as<raw_tlas_view>();
}

/// The view class the erased view carries — the active arm's (a tlas is always acceleration_structure).
/// A vacant element has none — it takes whatever the binding says — so gate on is_vacant() first.
[[nodiscard]] inline view_class view_class_of(raw_view const& v)
{
    return v.visit([](raw_buffer_view const& b) { return b.bound_as; },  //
                   [](raw_texture_view const& t) { return t.bound_as; }, //
                   [](raw_tlas_view const&) { return view_class::acceleration_structure; },
                   [](vacant_view const&)
                   {
                       CC_UNREACHABLE("a vacant element has no view class — gate on is_vacant() first");
                       return view_class::constants;
                   });
}

/// The layout the erased view carries: the buffer arm's `shape`, `texture` for a texture arm, `acceleration_structure` for a tlas arm.
/// A vacant element has none — it takes whatever the binding says — so gate on is_vacant() first.
[[nodiscard]] inline view_shape shape_of(raw_view const& v)
{
    return v.visit([](raw_buffer_view const& b) { return b.shape; },            //
                   [](raw_texture_view const&) { return view_shape::texture; }, //
                   [](raw_tlas_view const&) { return view_shape::acceleration_structure; },
                   [](vacant_view const&)
                   {
                       CC_UNREACHABLE("a vacant element has no shape — gate on is_vacant() first");
                       return view_shape::constants_block;
                   });
}

} // namespace sg

/// A constants block of `T` — a constant buffer / UBO binding, read-only.
template <sg::constants_element T>
struct sg::constants_buffer_view
{
    static constexpr view_class bound_as = view_class::constants;

    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize size_in_bytes = isize(sizeof(T));

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_buffer_view{
            .bound_as = bound_as,
            .shape = view_shape::constants_block,
            .buffer = buffer,
            .offset_in_bytes = offset_in_bytes,
            .size_in_bytes = size_in_bytes,
        };
    }

    operator raw_view() const { return to_raw(); }
};

/// A read-only storage view of an array of `T` — SRV / read SSBO.
/// With `T == byte` it is a byte-addressed view, shape `bytes`; otherwise a structured array strided by `sizeof(T)`.
template <sg::view_element T>
struct sg::readonly_buffer_view
{
    static constexpr view_class bound_as = view_class::readonly;

    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize element_count = 0; ///< count of `T` (for `byte`, a count of bytes)

    [[nodiscard]] raw_view to_raw() const
    {
        constexpr bool is_bytes = std::is_same_v<T, byte>;
        return raw_buffer_view{
            .bound_as = bound_as,
            .shape = is_bytes ? view_shape::bytes : view_shape::structured,
            .buffer = buffer,
            .offset_in_bytes = offset_in_bytes,
            .size_in_bytes = is_bytes ? element_count : 0,
            .element_count = is_bytes ? 0 : element_count,
            .stride_in_bytes = is_bytes ? 0 : isize(sizeof(T)),
        };
    }

    operator raw_view() const { return to_raw(); }
};

/// A read-write storage view of an array of `T` — UAV / read-write SSBO.
/// With `T == byte` it is a byte-addressed view, shape `bytes`; otherwise a structured array strided by `sizeof(T)`.
template <sg::view_element T>
struct sg::readwrite_buffer_view
{
    static constexpr view_class bound_as = view_class::readwrite;

    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize element_count = 0; ///< count of `T` (for `byte`, a count of bytes)

    [[nodiscard]] raw_view to_raw() const
    {
        constexpr bool is_bytes = std::is_same_v<T, byte>;
        return raw_buffer_view{
            .bound_as = bound_as,
            .shape = is_bytes ? view_shape::bytes : view_shape::structured,
            .buffer = buffer,
            .offset_in_bytes = offset_in_bytes,
            .size_in_bytes = is_bytes ? element_count : 0,
            .element_count = is_bytes ? 0 : element_count,
            .stride_in_bytes = is_bytes ? 0 : isize(sizeof(T)),
        };
    }

    operator raw_view() const { return to_raw(); }
};

/// A buffer view of `T` whose `bound_as` is known only at runtime — the erased middle between the typed leaves and `raw_view`.
/// Each leaf converts to it implicitly, and it erases on to `raw_view`.
/// For code that takes "any access of a buffer of `T`".
template <sg::view_element T>
struct sg::buffer_view
{
    view_class bound_as = view_class::readonly; ///< constants / readonly / readwrite — runtime, unlike the leaves
    view_shape shape = view_shape::structured;  ///< constants_block / structured / bytes
    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize size_in_bytes = 0;   ///< [constants_block, bytes]
    isize element_count = 0;   ///< [structured]
    isize stride_in_bytes = 0; ///< [structured] = sizeof(T)

    buffer_view() = default;

    /// From a raw buffer arm, and the entry point for tooling.
    /// The leaf conversions route through here, so the field mapping lives in one place — each leaf's `to_raw()`.
    explicit buffer_view(raw_buffer_view const& a)
      : bound_as(a.bound_as),
        shape(a.shape),
        buffer(a.buffer),
        offset_in_bytes(a.offset_in_bytes),
        size_in_bytes(a.size_in_bytes),
        element_count(a.element_count),
        stride_in_bytes(a.stride_in_bytes)
    {
    }

    buffer_view(readonly_buffer_view<T> const& v) : buffer_view(sg::as_buffer_view(v.to_raw())) {}
    buffer_view(readwrite_buffer_view<T> const& v) : buffer_view(sg::as_buffer_view(v.to_raw())) {}

    // Only where `T` is a constants_element.
    // The `U = T` template defers that, so `buffer_view<T>` stays well-formed for a non-uniform `T`, where naming `constants_buffer_view<T>` would be ill-formed.
    template <class U = T>
        requires(std::is_same_v<U, T> && constants_element<U>)
    buffer_view(constants_buffer_view<U> const& v) : buffer_view(sg::as_buffer_view(v.to_raw()))
    {
    }

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_buffer_view{.bound_as = bound_as,
                               .shape = shape,
                               .buffer = buffer,
                               .offset_in_bytes = offset_in_bytes,
                               .size_in_bytes = size_in_bytes,
                               .element_count = element_count,
                               .stride_in_bytes = stride_in_bytes};
    }

    operator raw_view() const { return to_raw(); }

    // Pin the runtime `bound_as` to a compile-time leaf — the inverse of the implicit leaf -> buffer_view conversions above.
    // Adds a check that `T` matches the view's layout, so a re-type with the wrong element size is a loud error rather than a silently wrong element count.
    [[nodiscard]] readonly_buffer_view<T> as_readonly() const
    {
        CC_ASSERT(bound_as == view_class::readonly, "buffer_view is not bound as readonly");
        _assert_element_matches();
        return {.buffer = buffer,
                .offset_in_bytes = offset_in_bytes,
                .element_count = std::is_same_v<T, byte> ? size_in_bytes : element_count};
    }
    [[nodiscard]] readwrite_buffer_view<T> as_readwrite() const
    {
        CC_ASSERT(bound_as == view_class::readwrite, "buffer_view is not bound as readwrite");
        _assert_element_matches();
        return {.buffer = buffer,
                .offset_in_bytes = offset_in_bytes,
                .element_count = std::is_same_v<T, byte> ? size_in_bytes : element_count};
    }
    // constants only where `T` obeys the constants block rules.
    // `U = T` defers that, so `buffer_view<T>` stays valid for a non-uniform `T`.
    template <class U = T>
        requires(std::is_same_v<U, T> && constants_element<U>)
    [[nodiscard]] constants_buffer_view<U> as_constants() const
    {
        CC_ASSERT(bound_as == view_class::constants && shape == view_shape::constants_block,
                  "buffer_view is not bound as "
                  "constants");
        return {.buffer = buffer, .offset_in_bytes = offset_in_bytes, .size_in_bytes = size_in_bytes};
    }
    [[nodiscard]] cc::optional<readonly_buffer_view<T>> try_as_readonly() const
    {
        if (bound_as != view_class::readonly)
            return {};
        return as_readonly();
    }
    [[nodiscard]] cc::optional<readwrite_buffer_view<T>> try_as_readwrite() const
    {
        if (bound_as != view_class::readwrite)
            return {};
        return as_readwrite();
    }
    template <class U = T>
        requires(std::is_same_v<U, T> && constants_element<U>)
    [[nodiscard]] cc::optional<constants_buffer_view<U>> try_as_constants() const
    {
        if (bound_as != view_class::constants || shape != view_shape::constants_block)
            return {};
        return as_constants();
    }

private:
    // `byte` is the byte-addressed `bytes` shape; any other `T` is a structured array whose stride is exactly `sizeof(T)`.
    // A mismatch means the caller picked the wrong element type, which the stride can prove, so it asserts even through `try_as_*`.
    void _assert_element_matches() const
    {
        if constexpr (std::is_same_v<T, byte>)
            CC_ASSERT(shape == view_shape::bytes, "recovering a buffer_view<byte> needs a bytes (byte-addressed) view");
        else
        {
            CC_ASSERT(shape == view_shape::structured, "recovering a buffer_view<T> (non-byte) needs a structured "
                                                       "view");
            CC_ASSERT(stride_in_bytes == isize(sizeof(T)), "buffer_view<T> element size does not match the view "
                                                           "stride");
        }
    }
};

// -- Texture views --
//    A texture view is typed by `Traits`, a `texture_view_traits<Dim>` naming the shader-facing dimension it binds as, where a buffer view is typed by its element `T`.
//    Only the dimension is compile-time; the texel `format` and subresource `range` stay runtime.
//    `texture<Traits>::as_*_view()` returns the precisely-typed leaf.

/// The compile-time shape of a texture *view*: the shader-facing dimension it binds as.
/// The single template argument of the typed texture view types.
/// Prefer the `tv_2d` / `tv_cube` / … aliases over spelling this out.
template <sg::texture_view_dimension Dim>
struct sg::texture_view_traits
{
    static constexpr texture_view_dimension dimension = Dim;
};

namespace sg
{

using tv_1d = texture_view_traits<texture_view_dimension::tex_1d>;
using tv_1d_array = texture_view_traits<texture_view_dimension::tex_1d_array>;
using tv_2d = texture_view_traits<texture_view_dimension::tex_2d>;
using tv_2d_array = texture_view_traits<texture_view_dimension::tex_2d_array>;
using tv_2d_ms = texture_view_traits<texture_view_dimension::tex_2d_ms>;
using tv_2d_ms_array = texture_view_traits<texture_view_dimension::tex_2d_ms_array>;
using tv_3d = texture_view_traits<texture_view_dimension::tex_3d>;
using tv_cube = texture_view_traits<texture_view_dimension::cube>;
using tv_cube_array = texture_view_traits<texture_view_dimension::cube_array>;

// Shape typedefs for the typed leaves — the ergonomic names, one per view dimension.
// An image's format stays a template argument, since it is the binding contract rather than a shape.
using texture_view_1d = texture_view<tv_1d>;
using texture_view_1d_array = texture_view<tv_1d_array>;
using texture_view_2d = texture_view<tv_2d>;
using texture_view_2d_array = texture_view<tv_2d_array>;
using texture_view_2d_ms = texture_view<tv_2d_ms>;
using texture_view_2d_ms_array = texture_view<tv_2d_ms_array>;
using texture_view_3d = texture_view<tv_3d>;
using texture_view_cube = texture_view<tv_cube>;
using texture_view_cube_array = texture_view<tv_cube_array>;

template <pixel_format Format>
using image_view_1d = image_view<tv_1d, Format>;
template <pixel_format Format>
using image_view_1d_array = image_view<tv_1d_array, Format>;
template <pixel_format Format>
using image_view_2d = image_view<tv_2d, Format>;
template <pixel_format Format>
using image_view_2d_array = image_view<tv_2d_array, Format>;
template <pixel_format Format>
using image_view_3d = image_view<tv_3d, Format>;

} // namespace sg

/// A sampled (SRV) texture view of dimension `Traits::dimension`, over a subresource range.
/// Built via `texture<Traits>::as_texture_view()` and the reinterpreting variants.
template <class Traits>
struct sg::texture_view
{
    static constexpr view_class bound_as = view_class::texture;
    static constexpr texture_view_dimension dimension = Traits::dimension;

    raw_texture_handle texture;
    pixel_format format = pixel_format::undefined;
    subresource_range range;

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_texture_view{.bound_as = bound_as,
                                .texture = texture,
                                .view_dimension = dimension,
                                .format = format,
                                .range = range};
    }

    operator raw_view() const { return to_raw(); }
};

/// A storage (UAV) image view of dimension `Traits::dimension` and texel format `Format`, over a single mip level.
/// The format is compile-time because it is the binding contract: WebGPU and vulkan require the view to be exactly the shader's declared format.
/// An image whose format is chosen at runtime is an `any_texture_view` instead, recovered with `as_image<Format>()` where the format is known.
/// The dimension must be an `image_view_dimension` — no cube, no MSAA.
/// Whether the shader reads, writes or both is the binding's `access`, not the view's.
/// Built via `texture<Traits>::as_image_view<Format>()` and friends.
template <class Traits, sg::pixel_format Format>
    requires sg::image_view_dimension<Traits::dimension>
struct sg::image_view
{
    static constexpr view_class bound_as = view_class::image;
    static constexpr texture_view_dimension dimension = Traits::dimension;
    static constexpr pixel_format format = Format;

    raw_texture_handle texture;
    subresource_range range;

    /// For a 3D image view: the half-open `[start, end)` window of depth slices the view exposes — D3D12's `FirstWSlice` / `WSize`.
    /// Depth slices are not subresources, since a whole 3D mip is one, so they live here rather than in `range`.
    /// Empty `{0, 0}` for every non-3D view.
    cc::start_end depth_slice_range = {.start = 0, .end = 0};

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_texture_view{.bound_as = bound_as,
                                .texture = texture,
                                .view_dimension = dimension,
                                .format = format,
                                .range = range,
                                .depth_slice_range = depth_slice_range};
    }

    operator raw_view() const { return to_raw(); }
};

/// A view of dimension `Traits::dimension` that is a texture or an image, decided only at runtime — the kind-erased middle between the typed leaves and `raw_view`.
/// Each leaf converts to it implicitly, and it erases on to `raw_view`.
/// For code that takes "a texture or an image of that dimension".
template <class Traits>
struct sg::any_texture_view
{
    static constexpr texture_view_dimension dimension = Traits::dimension;

    view_class bound_as = view_class::texture; ///< texture / image — runtime, unlike the leaves
    raw_texture_handle texture;
    pixel_format format = pixel_format::undefined;
    subresource_range range;
    cc::start_end depth_slice_range = {.start = 0, .end = 0};

    any_texture_view() = default;

    /// From a raw texture arm, and the entry point for tooling.
    /// The leaf conversions route through here.
    explicit any_texture_view(raw_texture_view const& a)
      : bound_as(a.bound_as), texture(a.texture), format(a.format), range(a.range), depth_slice_range(a.depth_slice_range)
    {
    }

    any_texture_view(texture_view<Traits> const& v) : any_texture_view(sg::as_texture_view(v.to_raw())) {}

    // An image exists only for an image dimension.
    // The `T = Traits` template defers that, so `any_texture_view<Traits>` stays well-formed for a cube or MS dimension.
    // Naming `image_view<Traits, …>` there would be ill-formed.
    template <class T = Traits, pixel_format Format>
        requires(std::is_same_v<T, Traits> && image_view_dimension<Traits::dimension>)
    any_texture_view(image_view<T, Format> const& v) : any_texture_view(sg::as_texture_view(v.to_raw()))
    {
    }

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_texture_view{.bound_as = bound_as,
                                .texture = texture,
                                .view_dimension = dimension,
                                .format = format,
                                .range = range,
                                .depth_slice_range = depth_slice_range};
    }

    operator raw_view() const { return to_raw(); }

    // Pin the runtime `bound_as` to a compile-time leaf — the inverse of the implicit leaf -> any_texture_view conversions above.
    // The dimension and range are already fixed, so a texture commits only `bound_as`, and an image its format too.
    [[nodiscard]] texture_view<Traits> as_texture() const
    {
        CC_ASSERT(bound_as == view_class::texture, "any_texture_view is not a texture");
        return {.texture = texture, .format = format, .range = range};
    }
    [[nodiscard]] cc::optional<texture_view<Traits>> try_as_texture() const
    {
        if (bound_as != view_class::texture)
            return {};
        return as_texture();
    }
    template <pixel_format Format, class T = Traits>
        requires(std::is_same_v<T, Traits> && image_view_dimension<Traits::dimension>)
    [[nodiscard]] image_view<T, Format> as_image() const
    {
        CC_ASSERT(bound_as == view_class::image, "any_texture_view is not an image");
        CC_ASSERT(format == Format, "any_texture_view is an image of a different format");
        return {.texture = texture, .range = range, .depth_slice_range = depth_slice_range};
    }
    template <pixel_format Format, class T = Traits>
        requires(std::is_same_v<T, Traits> && image_view_dimension<Traits::dimension>)
    [[nodiscard]] cc::optional<image_view<T, Format>> try_as_image() const
    {
        if (bound_as != view_class::image || format != Format)
            return {};
        return as_image<Format>();
    }
};

/// A ray-tracing acceleration structure (TLAS) bound as a shader resource — HLSL `RaytracingAccelerationStructure`.
/// It has no element type, no layout and no range, and carries the abstract `tlas` so each backend can bind it its own way.
/// Obtain one from `tlas::as_view()`.
///
/// **A null `tlas` is legal**, and binds the null acceleration structure: every `TraceRay` against it misses.
/// That is what a scene with no geometry yet wants — the miss shader runs and paints the environment — and it needs
/// no structure built and nothing allocated, so "nothing to trace" costs nothing rather than needing a stand-in.
struct sg::tlas_view
{
    static constexpr view_class bound_as = view_class::acceleration_structure;

    tlas_handle tlas; ///< the top-level acceleration structure to bind; null binds the null acceleration structure

    [[nodiscard]] raw_view to_raw() const { return raw_tlas_view{.tlas = tlas}; }

    operator raw_view() const { return to_raw(); }
};

/// Render-target / depth-stencil views — a texture bound as a color or depth-stencil target of a graphics pipeline.
/// These are not shader-visible, never enter a binding group or descriptor table, and are bound through the output-merger stage.
/// So they do not erase to `raw_view`: a backend consumes the typed view directly.
/// Built via `texture<Traits>::as_render_target_view()` / `as_depth_stencil_view()`.

/// A render-target view over a single mip level and array-slice range.
/// The texture's format must be a renderable color format.
/// Keeps the viewed texture alive via the held handle.
class sg::render_target_view
{
public:
    render_target_view() = default;
    render_target_view(raw_texture_handle texture,
                       texture_view_dimension dimension,
                       pixel_format format,
                       subresource_range range)
      : _texture(cc::move(texture)), _dimension(dimension), _format(format), _range(range)
    {
    }

    [[nodiscard]] raw_texture_handle const& texture() const { return _texture; }
    [[nodiscard]] texture_view_dimension dimension() const { return _dimension; }
    [[nodiscard]] pixel_format format() const { return _format; }
    [[nodiscard]] subresource_range const& range() const { return _range; }

    /// Pixel size of the viewed mip level (mip-adjusted, clamped to at least 1).
    [[nodiscard]] int width() const
    {
        int const w = _texture->width() >> _range.mip_range.start;
        return w < 1 ? 1 : w;
    }
    [[nodiscard]] int height() const
    {
        int const h = _texture->height() >> _range.mip_range.start;
        return h < 1 ? 1 : h;
    }

    /// (width, height) of the viewed mip level — the size to drive a dispatch or a projection with.
    [[nodiscard]] tg::vec2i size() const { return tg::vec2i(this->width(), this->height()); }

    /// width / height of the viewed mip level.
    /// Both are clamped to >= 1, so this never divides by zero.
    [[nodiscard]] float aspect_ratio() const { return float(this->width()) / float(this->height()); }

    // Bind as a rendering-scope target, choosing what happens to its contents at pass start.
    // The `&&` overloads move the view into the target; the `const&` overloads copy.
    // See command_list/raster.hh.
    [[nodiscard]] color_target cleared(tg::vec4f color) const&;
    [[nodiscard]] color_target cleared(tg::vec4f color) &&;
    [[nodiscard]] color_target preserved() const&;
    [[nodiscard]] color_target preserved() &&;
    [[nodiscard]] color_target discarded() const&;
    [[nodiscard]] color_target discarded() &&;

private:
    raw_texture_handle _texture;
    texture_view_dimension _dimension = texture_view_dimension::tex_2d;
    pixel_format _format = pixel_format::undefined;
    subresource_range _range;
};

/// A depth-stencil view over a single mip level and array-slice range.
/// The texture's format must be a depth or depth-stencil format.
/// Keeps the viewed texture alive.
class sg::depth_stencil_view
{
public:
    depth_stencil_view() = default;
    depth_stencil_view(raw_texture_handle texture,
                       texture_view_dimension dimension,
                       pixel_format format,
                       subresource_range range)
      : _texture(cc::move(texture)), _dimension(dimension), _format(format), _range(range)
    {
    }

    [[nodiscard]] raw_texture_handle const& texture() const { return _texture; }
    [[nodiscard]] texture_view_dimension dimension() const { return _dimension; }
    [[nodiscard]] pixel_format format() const { return _format; }
    [[nodiscard]] subresource_range const& range() const { return _range; }

    /// Pixel size of the viewed mip level (mip-adjusted, clamped to at least 1).
    [[nodiscard]] int width() const
    {
        int const w = _texture->width() >> _range.mip_range.start;
        return w < 1 ? 1 : w;
    }
    [[nodiscard]] int height() const
    {
        int const h = _texture->height() >> _range.mip_range.start;
        return h < 1 ? 1 : h;
    }

    /// (width, height) of the viewed mip level — matches the color targets it is paired with.
    [[nodiscard]] tg::vec2i size() const { return tg::vec2i(this->width(), this->height()); }

    /// width / height of the viewed mip level.
    /// Both are clamped to >= 1, so this never divides by zero.
    [[nodiscard]] float aspect_ratio() const { return float(this->width()) / float(this->height()); }

    // Bind as a rendering-scope target, choosing what happens to its contents at pass start.
    // The `&&` overloads move the view into the target; the `const&` overloads copy.
    // See command_list/raster.hh.
    [[nodiscard]] depth_stencil_target cleared(float depth, u8 stencil = 0) const&;
    [[nodiscard]] depth_stencil_target cleared(float depth, u8 stencil = 0) &&;
    [[nodiscard]] depth_stencil_target preserved() const&;
    [[nodiscard]] depth_stencil_target preserved() &&;
    [[nodiscard]] depth_stencil_target discarded() const&;
    [[nodiscard]] depth_stencil_target discarded() &&;

private:
    raw_texture_handle _texture;
    texture_view_dimension _dimension = texture_view_dimension::tex_2d;
    pixel_format _format = pixel_format::undefined;
    subresource_range _range;
};

namespace sg
{

// -- Erased arm -> typed leaf --
//    Declared on the arms above, defined here now the typed views exist.
//    Each delegates to the erased middle, which does the view-class check and the field mapping.

template <sg::view_element T>
auto raw_buffer_view::as_readonly() const
{
    return buffer_view<T>(*this).as_readonly();
}
template <sg::view_element T>
auto raw_buffer_view::as_readwrite() const
{
    return buffer_view<T>(*this).as_readwrite();
}
template <sg::constants_element T>
auto raw_buffer_view::as_constants() const
{
    return buffer_view<T>(*this).as_constants();
}
template <sg::view_element T>
auto raw_buffer_view::try_as_readonly() const
{
    return buffer_view<T>(*this).try_as_readonly();
}
template <sg::view_element T>
auto raw_buffer_view::try_as_readwrite() const
{
    return buffer_view<T>(*this).try_as_readwrite();
}
template <sg::constants_element T>
auto raw_buffer_view::try_as_constants() const
{
    return buffer_view<T>(*this).try_as_constants();
}

template <class Traits>
auto raw_texture_view::as_texture() const
{
    CC_ASSERT(view_dimension == Traits::dimension, "raw_texture_view dimension does not match Traits");
    return any_texture_view<Traits>(*this).as_texture();
}
template <class Traits, sg::pixel_format Format>
    requires sg::image_view_dimension<Traits::dimension>
auto raw_texture_view::as_image() const
{
    CC_ASSERT(view_dimension == Traits::dimension, "raw_texture_view dimension does not match Traits");
    return any_texture_view<Traits>(*this).template as_image<Format>();
}
template <class Traits>
auto raw_texture_view::try_as_texture() const
{
    if (view_dimension != Traits::dimension)
        return cc::optional<texture_view<Traits>>{};
    return any_texture_view<Traits>(*this).try_as_texture();
}
template <class Traits, sg::pixel_format Format>
    requires sg::image_view_dimension<Traits::dimension>
auto raw_texture_view::try_as_image() const
{
    if (view_dimension != Traits::dimension)
        return cc::optional<image_view<Traits, Format>>{};
    return any_texture_view<Traits>(*this).template try_as_image<Format>();
}

// -- `raw_view` -> typed leaf, in a single call --
//    Each `get_if`s the matching resource arm and re-types it, so the arm being a different resource kind is the one failure these add.
//    `as_*` assert on it; `try_as_*` return nullopt for it, as they do for a mismatched view class.

template <sg::view_element T>
[[nodiscard]] readonly_buffer_view<T> as_readonly_buffer(raw_view const& v)
{
    auto const* a = sg::try_as_buffer_view(v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a buffer arm");
    return a->as_readonly<T>();
}
template <sg::view_element T>
[[nodiscard]] readwrite_buffer_view<T> as_readwrite_buffer(raw_view const& v)
{
    auto const* a = sg::try_as_buffer_view(v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a buffer arm");
    return a->as_readwrite<T>();
}
template <sg::constants_element T>
[[nodiscard]] constants_buffer_view<T> as_constants_buffer(raw_view const& v)
{
    auto const* a = sg::try_as_buffer_view(v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a buffer arm");
    return a->as_constants<T>();
}
template <sg::view_element T>
[[nodiscard]] cc::optional<readonly_buffer_view<T>> try_as_readonly_buffer(raw_view const& v)
{
    if (auto const* a = sg::try_as_buffer_view(v))
        return a->try_as_readonly<T>();
    return {};
}
template <sg::view_element T>
[[nodiscard]] cc::optional<readwrite_buffer_view<T>> try_as_readwrite_buffer(raw_view const& v)
{
    if (auto const* a = sg::try_as_buffer_view(v))
        return a->try_as_readwrite<T>();
    return {};
}
template <sg::constants_element T>
[[nodiscard]] cc::optional<constants_buffer_view<T>> try_as_constants_buffer(raw_view const& v)
{
    if (auto const* a = sg::try_as_buffer_view(v))
        return a->try_as_constants<T>();
    return {};
}

template <class Traits>
[[nodiscard]] texture_view<Traits> as_texture(raw_view const& v)
{
    auto const* a = sg::try_as_texture_view(v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a texture arm");
    return a->as_texture<Traits>();
}
template <class Traits, sg::pixel_format Format>
    requires sg::image_view_dimension<Traits::dimension>
[[nodiscard]] image_view<Traits, Format> as_image(raw_view const& v)
{
    auto const* a = sg::try_as_texture_view(v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a texture arm");
    return a->as_image<Traits, Format>();
}
template <class Traits>
[[nodiscard]] cc::optional<texture_view<Traits>> try_as_texture(raw_view const& v)
{
    if (auto const* a = sg::try_as_texture_view(v))
        return a->try_as_texture<Traits>();
    return {};
}
template <class Traits, sg::pixel_format Format>
    requires sg::image_view_dimension<Traits::dimension>
[[nodiscard]] cc::optional<image_view<Traits, Format>> try_as_image(raw_view const& v)
{
    if (auto const* a = sg::try_as_texture_view(v))
        return a->try_as_image<Traits, Format>();
    return {};
}
} // namespace sg
