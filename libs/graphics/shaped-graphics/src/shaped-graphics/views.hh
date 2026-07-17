#pragma once

#include <clean-core/common/assert.hh>            // CC_ASSERT (access checks on the raw -> typed recovery)
#include <clean-core/common/utility.hh>           // cc::move
#include <clean-core/error/optional.hh>           // cc::optional (try_as_* recovery)
#include <shaped-graphics/backend/subresource.hh> // subresource_range (texture view sub-selection)
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh> // render_target_view / depth_stencil_view read the texture's extent
#include <typed-geometry/linalg/vec.hh>   // tg::vec4f (clear color builder)

#include <type_traits>
#include <variant>

/// Strongly-typed resource *views*: a lightweight, typed handle onto a (sub-range of a) buffer,
/// interpreted as a shader-facing binding. A routine takes exactly the view it operates on instead of
/// a raw resource plus overloads. See libs/graphics/shaped-graphics/docs/concepts/views.md.

namespace sg
{
/// A view's element (`readonly`/`readwrite`) or block (`uniform`) type. GPUs load at 4-byte (DWORD)
/// alignment, so it must be `byte` (the raw / byte-addressed path) or a multiple of 4 bytes.
template <class T>
concept view_element = std::is_same_v<T, cc::byte> || (sizeof(T) % 4 == 0);

/// Placement rules a uniform (constant) buffer view must satisfy: its byte offset is a multiple of
/// `uniform_buffer_offset_alignment`, and its size a multiple of 16 (std140 packing) and at most
/// `max_uniform_buffer_size`. Cross-backend rationale for the values is in the views concept doc.
constexpr isize uniform_buffer_offset_alignment = 256; // Vk minUniformBufferOffsetAlignment / WGPU / DX12 CBV placement
constexpr isize max_uniform_buffer_size = 65536;       // 64 KiB — DX12 max CBV / WGPU max uniform binding

/// Placement rules a shader-facing *storage* buffer view (readonly / readwrite; raw or structured) must
/// satisfy. A view is a *subrange* of a buffer, so it carries the binding-offset rules — `buffer<T>` itself
/// is a whole buffer recast like a span and has none of these. Hardcoded portable floors rather than
/// per-device queries, same approach as `uniform_buffer_offset_alignment`.
///
///   - offset: 256 bytes. WebGPU's `minStorageBufferOffsetAlignment` is 256, and Vulkan lets an
///     implementation require up to 256 (its required-limit *maximum*, so some hardware really does), making
///     256 the only portable value. D3D12 adds an orthogonal rule, asserted separately: a structured view
///     addresses by element index (`FirstElement = offset / stride`), so the offset must *also* be a
///     multiple of the stride.
///   - size: 4 bytes. A WebGPU storage binding's size must be a multiple of 4.
///
/// Deliberate escape hatch: build the erased `raw_buffer_view` aggregate yourself to bypass these when you
/// knowingly target only backends with looser rules.
constexpr isize storage_buffer_offset_alignment = 256;
constexpr isize storage_buffer_size_alignment = 4;

/// A `uniform_buffer_view` block type: a `view_element` whose size obeys the uniform block rules above (a
/// multiple of 16, at most 64 KiB). Excludes `byte` — a uniform block of raw bytes is meaningless.
template <class T>
concept uniform_element = view_element<T> && (sizeof(T) % 16 == 0) && (isize(sizeof(T)) <= max_uniform_buffer_size);

/// How a shader reads a view. Mirrors buffer_usage's uniform/readonly/readwrite split.
enum class view_class
{
    uniform,                ///< uniform block — constant buffer / UBO (read-only)
    readonly,               ///< read-only storage — SRV / read SSBO / sampled texture
    readwrite,              ///< read-write storage — UAV / read-write SSBO / storage texture
    acceleration_structure, ///< ray-tracing TLAS — a read-only SRV addressed by GPU VA (no bound resource)
    // Future (with a graphics pipeline / samplers): render_target, depth_stencil, sampler.
};

/// How a view's bytes are laid out. `raw` is byte-addressed (element type `byte`); `structured` is an
/// array strided by the element type; `uniform_block` is a single struct block; `texture` is a texel grid
/// (dimension / array / cube / samples come from the bound raw_texture's description).
enum class view_shape
{
    uniform_block,
    structured,
    raw,
    texture,
    acceleration_structure, ///< a ray-tracing TLAS bound as an SRV — no byte layout, addressed by the AS's GPU VA
    // Future (with formats): texel (a typed buffer view).
};

/// The shader-facing dimensionality of a texture view — how the bound texels are declared in the shader
/// (HLSL `Texture2D` / `Texture2DArray` / `TextureCube` / …; Vulkan `VkImageViewType`; D3D `SRV/UAV_DIMENSION`).
/// Distinct from the texture's own `texture_dimension`: it is a *reinterpretation* the view chooses, so a
/// single slice of a 2D array is `tex_2d`, a cube face is `tex_2d`, one cube of a cube array is `cube`.
/// Storage (UAV) views only use the non-cube, non-multisampled members (a cube UAV is a 2D array).
enum class texture_view_dimension : u8
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

/// A dimension a storage (UAV) view may bind as: no cube, no multisampling (a cube UAV is a 2D array; MSAA
/// has no UAV). Constrains `readwrite_texture_view` and the storage-view recovery paths. Declared here (not
/// beside those types) so the erased `raw_texture_view` arm can name it in its recovery accessors.
template <texture_view_dimension Dim>
concept storage_view_dimension
    = Dim != texture_view_dimension::cube && Dim != texture_view_dimension::cube_array
   && Dim != texture_view_dimension::tex_2d_ms && Dim != texture_view_dimension::tex_2d_ms_array;

// The erased form every typed view converts into is `raw_view` (below) — a sum over one cohesive payload
// per resource kind. The three payload arms are also the directly-usable "raw" binding vocabulary for
// tooling that builds bindings without the typed wrappers.

/// A buffer view's erased payload: the access class, byte layout, and buffer a backend reads to build a
/// CBV / SRV / UAV. `shape` picks the interpretation (uniform block / structured array / raw bytes).
struct raw_buffer_view
{
    view_class access = view_class::readonly;  ///< uniform / readonly / readwrite
    view_shape shape = view_shape::structured; ///< uniform_block / structured / raw
    raw_buffer_handle buffer;                  ///< the viewed buffer
    isize offset_in_bytes = 0;                 ///< start of the view within the buffer
    isize size_in_bytes = 0;                   ///< [uniform_block, raw] visible byte size
    isize element_count = 0;                   ///< [structured] number of elements
    isize stride_in_bytes = 0;                 ///< [structured] element stride (= sizeof(T))

    // Re-type this erased arm as a strongly-typed leaf of element `T` (you supply `T`, an unchecked
    // reinterpret of the bytes). Asserts the access class matches; `try_as_*` returns nullopt on mismatch.
    // Delegate to `buffer_view<T>` for the field mapping — defined below it. (Member function templates keep
    // the struct an aggregate, so brace/designated init is unaffected.)
    template <view_element T>
    [[nodiscard]] auto as_readonly() const; // -> readonly_buffer_view<T>
    template <view_element T>
    [[nodiscard]] auto as_readwrite() const; // -> readwrite_buffer_view<T>
    template <uniform_element T>
    [[nodiscard]] auto as_uniform() const; // -> uniform_buffer_view<T>
    template <view_element T>
    [[nodiscard]] auto try_as_readonly() const; // -> cc::optional<readonly_buffer_view<T>>
    template <view_element T>
    [[nodiscard]] auto try_as_readwrite() const; // -> cc::optional<readwrite_buffer_view<T>>
    template <uniform_element T>
    [[nodiscard]] auto try_as_uniform() const; // -> cc::optional<uniform_buffer_view<T>>
};

/// A texture view's erased payload: the sampled (SRV) / storage (UAV) descriptor a backend builds over a
/// subresource range. Dimension / format are a reinterpretation the view chose, not the texture's shape.
struct raw_texture_view
{
    view_class access = view_class::readonly;                               ///< readonly (SRV) / readwrite (UAV)
    raw_texture_handle texture;                                             ///< the viewed texture
    texture_view_dimension view_dimension = texture_view_dimension::tex_2d; ///< shader-facing SRV/UAV dimension
    pixel_format format = pixel_format::undefined; ///< the format the descriptor reads/writes as
    subresource_range range;                       ///< the mip × array-slice × aspect sub-range the view exposes
    cc::start_end depth_slice_range
        = {.start = 0, .end = 0}; ///< [3D storage view] depth (W/Z) slice window; empty otherwise

    // Re-type this erased arm as a strongly-typed leaf of shape `Traits` (you supply `Traits`). Asserts the
    // view dimension and access class match; `try_as_*` returns nullopt on mismatch. Delegate to
    // `texture_view<Traits>` for the field mapping — defined below it.
    template <class Traits>
    [[nodiscard]] auto as_readonly() const; // -> readonly_texture_view<Traits>
    template <class Traits>
        requires storage_view_dimension<Traits::dimension>
    [[nodiscard]] auto as_readwrite() const; // -> readwrite_texture_view<Traits>
    template <class Traits>
    [[nodiscard]] auto try_as_readonly() const; // -> cc::optional<readonly_texture_view<Traits>>
    template <class Traits>
        requires storage_view_dimension<Traits::dimension>
    [[nodiscard]] auto try_as_readwrite() const; // -> cc::optional<readwrite_texture_view<Traits>>
};

/// An acceleration-structure view's erased payload: the abstract TLAS a backend binds its own way (dx12 by
/// the AS's storage GPU VA; vulkan by the native VkAccelerationStructureKHR handle). Its access class is
/// always acceleration_structure.
struct raw_tlas_view
{
    tlas_handle tlas; ///< the viewed top-level acceleration structure
};

/// The erased form every typed view converts into — a sum over the per-resource payloads. A backend
/// `std::visit`s it (or `get_if`s an arm) to build the native descriptor; `named_view` carries one.
/// NOTE: `std::variant` for now — likely a `cc::variant` once that lands.
using raw_view = std::variant<raw_buffer_view, raw_texture_view, raw_tlas_view>;

/// The access class the erased view carries — the active arm's (a tlas is always acceleration_structure).
[[nodiscard]] inline view_class access_of(raw_view const& v)
{
    if (auto const* b = std::get_if<raw_buffer_view>(&v))
        return b->access;
    if (auto const* t = std::get_if<raw_texture_view>(&v))
        return t->access;
    return view_class::acceleration_structure;
}

/// The layout the erased view carries — the buffer arm's `shape`; a texture arm is `texture`, a tlas arm
/// `acceleration_structure`.
[[nodiscard]] inline view_shape shape_of(raw_view const& v)
{
    if (auto const* b = std::get_if<raw_buffer_view>(&v))
        return b->shape;
    if (std::holds_alternative<raw_texture_view>(v))
        return view_shape::texture;
    return view_shape::acceleration_structure;
}

/// A uniform block of `T` — a constant buffer / UBO binding (read-only). {buffer, offset, sizeof(T)}.
template <uniform_element T>
struct uniform_buffer_view
{
    static constexpr view_class access = view_class::uniform;

    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize size_in_bytes = isize(sizeof(T));

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_buffer_view{
            .access = access,
            .shape = view_shape::uniform_block,
            .buffer = buffer,
            .offset_in_bytes = offset_in_bytes,
            .size_in_bytes = size_in_bytes,
        };
    }

    operator raw_view() const { return to_raw(); }
};

/// A read-only storage view of an array of `T` (SRV / read SSBO). With `T == byte` it is a raw,
/// byte-addressed view; otherwise a structured array strided by `sizeof(T)`.
template <view_element T>
struct readonly_buffer_view
{
    static constexpr view_class access = view_class::readonly;

    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize element_count = 0; ///< count of `T` (for `byte`, a count of bytes)

    [[nodiscard]] raw_view to_raw() const
    {
        constexpr bool is_raw = std::is_same_v<T, cc::byte>;
        return raw_buffer_view{
            .access = access,
            .shape = is_raw ? view_shape::raw : view_shape::structured,
            .buffer = buffer,
            .offset_in_bytes = offset_in_bytes,
            .size_in_bytes = is_raw ? element_count : 0,
            .element_count = is_raw ? 0 : element_count,
            .stride_in_bytes = is_raw ? 0 : isize(sizeof(T)),
        };
    }

    operator raw_view() const { return to_raw(); }
};

/// A read-write storage view of an array of `T` (UAV / read-write SSBO). With `T == byte` it is a
/// raw, byte-addressed view; otherwise a structured array strided by `sizeof(T)`.
template <view_element T>
struct readwrite_buffer_view
{
    static constexpr view_class access = view_class::readwrite;

    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize element_count = 0; ///< count of `T` (for `byte`, a count of bytes)

    [[nodiscard]] raw_view to_raw() const
    {
        constexpr bool is_raw = std::is_same_v<T, cc::byte>;
        return raw_buffer_view{
            .access = access,
            .shape = is_raw ? view_shape::raw : view_shape::structured,
            .buffer = buffer,
            .offset_in_bytes = offset_in_bytes,
            .size_in_bytes = is_raw ? element_count : 0,
            .element_count = is_raw ? 0 : element_count,
            .stride_in_bytes = is_raw ? 0 : isize(sizeof(T)),
        };
    }

    operator raw_view() const { return to_raw(); }
};

/// A buffer view of `T` with its access class known only at runtime — the resource-typed, access-erased
/// middle between the fully-typed leaves (uniform / readonly / readwrite_buffer_view<T>) and the fully
/// erased raw_view. Each leaf converts to it implicitly; it mirrors the erased buffer fields (its `access`
/// is runtime) and erases on to raw_view. For code that takes "any access of a buffer of T".
template <view_element T>
struct buffer_view
{
    view_class access = view_class::readonly;  ///< uniform / readonly / readwrite — runtime, unlike the leaves
    view_shape shape = view_shape::structured; ///< uniform_block / structured / raw
    raw_buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize size_in_bytes = 0;   ///< [uniform_block, raw]
    isize element_count = 0;   ///< [structured]
    isize stride_in_bytes = 0; ///< [structured] = sizeof(T)

    buffer_view() = default;

    /// From a raw buffer arm (also the tooling entry point). The leaf conversions route through here so the
    /// field mapping lives in one place (each leaf's to_raw()).
    explicit buffer_view(raw_buffer_view const& a)
      : access(a.access),
        shape(a.shape),
        buffer(a.buffer),
        offset_in_bytes(a.offset_in_bytes),
        size_in_bytes(a.size_in_bytes),
        element_count(a.element_count),
        stride_in_bytes(a.stride_in_bytes)
    {
    }

    buffer_view(readonly_buffer_view<T> const& v) : buffer_view(std::get<raw_buffer_view>(v.to_raw())) {}
    buffer_view(readwrite_buffer_view<T> const& v) : buffer_view(std::get<raw_buffer_view>(v.to_raw())) {}

    // Only where `T` is a uniform_element; the `U = T` template defers so buffer_view<T> stays well-formed
    // for a non-uniform `T` (naming uniform_buffer_view<T> there would be ill-formed).
    template <class U = T>
        requires(std::is_same_v<U, T> && uniform_element<U>)
    buffer_view(uniform_buffer_view<U> const& v) : buffer_view(std::get<raw_buffer_view>(v.to_raw()))
    {
    }

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_buffer_view{.access = access,
                               .shape = shape,
                               .buffer = buffer,
                               .offset_in_bytes = offset_in_bytes,
                               .size_in_bytes = size_in_bytes,
                               .element_count = element_count,
                               .stride_in_bytes = stride_in_bytes};
    }

    operator raw_view() const { return to_raw(); }

    // Pin the runtime `access` to a compile-time leaf — the inverse of the implicit leaf -> buffer_view
    // conversions above. `as_*` asserts the access class matches; `try_as_*` returns nullopt on mismatch. The
    // element type `T` and byte fields are already fixed, so only the access class is being committed. (A raw
    // byte view keeps its element count in `size_in_bytes`; a structured view in `element_count`.) `as_*` also
    // asserts `T` matches the view's layout — `T == byte` <-> a raw view, otherwise sizeof(T) == the stride —
    // so a re-type with the wrong element size is a loud error, not a silently wrong element count.
    [[nodiscard]] readonly_buffer_view<T> as_readonly() const
    {
        CC_ASSERT(access == view_class::readonly, "buffer_view access is not readonly");
        _assert_element_matches();
        return {.buffer = buffer,
                .offset_in_bytes = offset_in_bytes,
                .element_count = std::is_same_v<T, cc::byte> ? size_in_bytes : element_count};
    }
    [[nodiscard]] readwrite_buffer_view<T> as_readwrite() const
    {
        CC_ASSERT(access == view_class::readwrite, "buffer_view access is not readwrite");
        _assert_element_matches();
        return {.buffer = buffer,
                .offset_in_bytes = offset_in_bytes,
                .element_count = std::is_same_v<T, cc::byte> ? size_in_bytes : element_count};
    }
    // uniform only where T obeys the uniform block rules (U = T defers so buffer_view<T> stays valid for a
    // non-uniform T — naming uniform_buffer_view<T> there would be ill-formed).
    template <class U = T>
        requires(std::is_same_v<U, T> && uniform_element<U>)
    [[nodiscard]] uniform_buffer_view<U> as_uniform() const
    {
        CC_ASSERT(access == view_class::uniform && shape == view_shape::uniform_block, "buffer_view access is not "
                                                                                       "uniform");
        return {.buffer = buffer, .offset_in_bytes = offset_in_bytes, .size_in_bytes = size_in_bytes};
    }
    [[nodiscard]] cc::optional<readonly_buffer_view<T>> try_as_readonly() const
    {
        if (access != view_class::readonly)
            return {};
        return as_readonly();
    }
    [[nodiscard]] cc::optional<readwrite_buffer_view<T>> try_as_readwrite() const
    {
        if (access != view_class::readwrite)
            return {};
        return as_readwrite();
    }
    template <class U = T>
        requires(std::is_same_v<U, T> && uniform_element<U>)
    [[nodiscard]] cc::optional<uniform_buffer_view<U>> try_as_uniform() const
    {
        if (access != view_class::uniform || shape != view_shape::uniform_block)
            return {};
        return as_uniform();
    }

private:
    // A readonly/readwrite recovery re-types the bytes, so `T` must match the view's layout: `byte` is the
    // raw (byte-addressed) shape; any other `T` is a structured array whose stride is exactly sizeof(T). A
    // mismatch means the caller picked the wrong element type (the stride tells the real size) — a contract
    // bug, so it asserts even through `try_as_*` (which only tolerates the runtime access-class variance).
    void _assert_element_matches() const
    {
        if constexpr (std::is_same_v<T, cc::byte>)
            CC_ASSERT(shape == view_shape::raw, "recovering a buffer_view<byte> needs a raw (byte-addressed) view");
        else
        {
            CC_ASSERT(shape == view_shape::structured, "recovering a buffer_view<T> (non-byte) needs a structured "
                                                       "view");
            CC_ASSERT(stride_in_bytes == isize(sizeof(T)), "buffer_view<T> element size does not match the view "
                                                           "stride");
        }
    }
};

// -- Texture views. Unlike buffer views (typed by element `T`), a texture view is typed by `Traits` — a
//    `texture_view_traits<Dim>` naming the shader-facing dimension it binds as (Texture2D / TextureCube /
//    Texture2DArray / …). Only the dimension is compile-time; the texel `format` and subresource `range`
//    stay runtime. `texture<Traits>::as_*_view()` returns the precisely-typed leaf.

/// The compile-time shape of a texture *view* — the shader-facing dimension it binds as, a reinterpretation
/// the view chose (distinct from the texture's own dimension). The single template argument of the typed
/// texture view types. Prefer the `tv_2d` / `tv_cube` / … aliases over spelling this.
template <texture_view_dimension Dim>
struct texture_view_traits
{
    static constexpr texture_view_dimension dimension = Dim;
};

using tv_1d = texture_view_traits<texture_view_dimension::tex_1d>;
using tv_1d_array = texture_view_traits<texture_view_dimension::tex_1d_array>;
using tv_2d = texture_view_traits<texture_view_dimension::tex_2d>;
using tv_2d_array = texture_view_traits<texture_view_dimension::tex_2d_array>;
using tv_2d_ms = texture_view_traits<texture_view_dimension::tex_2d_ms>;
using tv_2d_ms_array = texture_view_traits<texture_view_dimension::tex_2d_ms_array>;
using tv_3d = texture_view_traits<texture_view_dimension::tex_3d>;
using tv_cube = texture_view_traits<texture_view_dimension::cube>;
using tv_cube_array = texture_view_traits<texture_view_dimension::cube_array>;

/// A read-only (sampled / SRV) texture view of shader-facing dimension `Traits::dimension`, over a
/// subresource range. Built via `texture<Traits>::as_readonly_view()` and the reinterpreting variants.
template <class Traits>
struct readonly_texture_view
{
    static constexpr view_class access = view_class::readonly;
    static constexpr texture_view_dimension dimension = Traits::dimension;

    raw_texture_handle texture;
    pixel_format format = pixel_format::undefined;
    subresource_range range;

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_texture_view{.access = access,
                                .texture = texture,
                                .view_dimension = dimension,
                                .format = format,
                                .range = range};
    }

    operator raw_view() const { return to_raw(); }
};

/// A read-write (storage / UAV) texture view of dimension `Traits::dimension`, over a single mip level. The
/// dimension must be a `storage_view_dimension` (no cube / no MSAA). Built via
/// `texture<Traits>::as_readwrite_view()` and friends.
template <class Traits>
    requires storage_view_dimension<Traits::dimension>
struct readwrite_texture_view
{
    static constexpr view_class access = view_class::readwrite;
    static constexpr texture_view_dimension dimension = Traits::dimension;

    raw_texture_handle texture;
    pixel_format format = pixel_format::undefined;
    subresource_range range;

    /// For a 3D storage view (`dimension == tex_3d`): the half-open `[start, end)` window of depth slices
    /// (a 3D texture's W / Z axis — D3D12's `FirstWSlice`/`WSize`) the view exposes. Not subresources (a
    /// whole 3D mip is one), so they live here, not in `range`. Empty `{0, 0}` for every non-3D view.
    cc::start_end depth_slice_range = {.start = 0, .end = 0};

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_texture_view{.access = access,
                                .texture = texture,
                                .view_dimension = dimension,
                                .format = format,
                                .range = range,
                                .depth_slice_range = depth_slice_range};
    }

    operator raw_view() const { return to_raw(); }
};

/// A texture view of shader-facing dimension `Traits::dimension` with its access class known only at
/// runtime — the access-erased middle between the fully-typed leaves (readonly / readwrite_texture_view
/// <Traits>) and raw_view. Each leaf converts to it implicitly; it erases on to raw_view. For code that
/// takes "any access of a texture view of that dimension".
template <class Traits>
struct texture_view
{
    static constexpr texture_view_dimension dimension = Traits::dimension;

    view_class access = view_class::readonly; ///< readonly / readwrite — runtime, unlike the leaves
    raw_texture_handle texture;
    pixel_format format = pixel_format::undefined;
    subresource_range range;
    cc::start_end depth_slice_range = {.start = 0, .end = 0};

    texture_view() = default;

    /// From a raw texture arm (also the tooling entry point). Leaf conversions route through here.
    explicit texture_view(raw_texture_view const& a)
      : access(a.access), texture(a.texture), format(a.format), range(a.range), depth_slice_range(a.depth_slice_range)
    {
    }

    texture_view(readonly_texture_view<Traits> const& v) : texture_view(std::get<raw_texture_view>(v.to_raw())) {}

    // readwrite exists only for a storage dimension; the `T = Traits` template defers so texture_view<Traits>
    // stays well-formed when Traits is a cube / MS dimension (naming readwrite_texture_view<Traits> is ill-formed there).
    template <class T = Traits>
        requires(std::is_same_v<T, Traits> && storage_view_dimension<Traits::dimension>)
    texture_view(readwrite_texture_view<T> const& v) : texture_view(std::get<raw_texture_view>(v.to_raw()))
    {
    }

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_texture_view{.access = access,
                                .texture = texture,
                                .view_dimension = dimension,
                                .format = format,
                                .range = range,
                                .depth_slice_range = depth_slice_range};
    }

    operator raw_view() const { return to_raw(); }

    // Pin the runtime `access` to a compile-time leaf — the inverse of the implicit leaf -> texture_view
    // conversions above. `as_readonly` asserts access is readonly; `as_readwrite` (storage dimensions only)
    // asserts readwrite. `try_as_*` return nullopt on mismatch. The shape (`Traits::dimension`), format, and
    // range are already fixed.
    [[nodiscard]] readonly_texture_view<Traits> as_readonly() const
    {
        CC_ASSERT(access == view_class::readonly, "texture_view access is not readonly");
        return {.texture = texture, .format = format, .range = range};
    }
    [[nodiscard]] cc::optional<readonly_texture_view<Traits>> try_as_readonly() const
    {
        if (access != view_class::readonly)
            return {};
        return as_readonly();
    }
    template <class T = Traits>
        requires(std::is_same_v<T, Traits> && storage_view_dimension<Traits::dimension>)
    [[nodiscard]] readwrite_texture_view<T> as_readwrite() const
    {
        CC_ASSERT(access == view_class::readwrite, "texture_view access is not readwrite");
        return {.texture = texture, .format = format, .range = range, .depth_slice_range = depth_slice_range};
    }
    template <class T = Traits>
        requires(std::is_same_v<T, Traits> && storage_view_dimension<Traits::dimension>)
    [[nodiscard]] cc::optional<readwrite_texture_view<T>> try_as_readwrite() const
    {
        if (access != view_class::readwrite)
            return {};
        return as_readwrite();
    }
};

/// A ray-tracing acceleration structure (TLAS) bound as a shader resource — HLSL
/// `RaytracingAccelerationStructure`. Unlike buffer / texture views it has no element type, no layout, and no
/// range; it carries the abstract `tlas` so each backend can bind it its own way (dx12 by the AS's GPU VA,
/// vulkan by the native VkAccelerationStructureKHR handle). Obtain one from `tlas::as_view()`.
struct tlas_view
{
    static constexpr view_class access = view_class::acceleration_structure;

    tlas_handle tlas; ///< the top-level acceleration structure to bind

    [[nodiscard]] raw_view to_raw() const { return raw_tlas_view{.tlas = tlas}; }

    operator raw_view() const { return to_raw(); }
};

/// Render-target / depth-stencil views — a texture bound as a color (render target) or depth-stencil target
/// of a graphics pipeline. Unlike the shader-binding views above, these are *not* shader-visible, never
/// enter a binding group / descriptor table, and are bound via the output-merger stage (OMSetRenderTargets /
/// dynamic-rendering attachments). They therefore do not erase to `raw_view`: a backend consumes the typed
/// view directly. Built via `texture<Traits>::as_render_target_view()` / `as_depth_stencil_view()`.

/// A render-target view over a single mip level and array-slice range. The texture's format must be a color
/// (renderable) format. Keeps the viewed texture alive via the held handle.
class render_target_view
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

    // Bind as a rendering-scope target, choosing what happens to its contents at pass start. `&&`
    // overloads move the view into the target; the `const&` overloads copy. See command_list.raster.hh.
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

/// A depth-stencil view over a single mip level and array-slice range. The texture's format must be a depth
/// (or depth-stencil) format. Keeps the viewed texture alive.
class depth_stencil_view
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

    // Bind as a rendering-scope target, choosing what happens to its contents at pass start. `&&`
    // overloads move the view into the target; the `const&` overloads copy. See command_list.raster.hh.
    [[nodiscard]] depth_stencil_target cleared(float depth, cc::u8 stencil = 0) const&;
    [[nodiscard]] depth_stencil_target cleared(float depth, cc::u8 stencil = 0) &&;
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

// -- Erased arm -> typed leaf (declared on the arms above; defined here now the typed views exist). Each
//    delegates to the access-erased middle, which does the access check + field mapping. The texture arm also
//    checks that the runtime `view_dimension` matches `Traits::dimension` (the middle drops it). --

template <view_element T>
auto raw_buffer_view::as_readonly() const
{
    return buffer_view<T>(*this).as_readonly();
}
template <view_element T>
auto raw_buffer_view::as_readwrite() const
{
    return buffer_view<T>(*this).as_readwrite();
}
template <uniform_element T>
auto raw_buffer_view::as_uniform() const
{
    return buffer_view<T>(*this).as_uniform();
}
template <view_element T>
auto raw_buffer_view::try_as_readonly() const
{
    return buffer_view<T>(*this).try_as_readonly();
}
template <view_element T>
auto raw_buffer_view::try_as_readwrite() const
{
    return buffer_view<T>(*this).try_as_readwrite();
}
template <uniform_element T>
auto raw_buffer_view::try_as_uniform() const
{
    return buffer_view<T>(*this).try_as_uniform();
}

template <class Traits>
auto raw_texture_view::as_readonly() const
{
    CC_ASSERT(view_dimension == Traits::dimension, "raw_texture_view dimension does not match Traits");
    return texture_view<Traits>(*this).as_readonly();
}
template <class Traits>
    requires storage_view_dimension<Traits::dimension>
auto raw_texture_view::as_readwrite() const
{
    CC_ASSERT(view_dimension == Traits::dimension, "raw_texture_view dimension does not match Traits");
    return texture_view<Traits>(*this).as_readwrite();
}
template <class Traits>
auto raw_texture_view::try_as_readonly() const
{
    if (view_dimension != Traits::dimension)
        return cc::optional<readonly_texture_view<Traits>>{};
    return texture_view<Traits>(*this).try_as_readonly();
}
template <class Traits>
    requires storage_view_dimension<Traits::dimension>
auto raw_texture_view::try_as_readwrite() const
{
    if (view_dimension != Traits::dimension)
        return cc::optional<readwrite_texture_view<Traits>>{};
    return texture_view<Traits>(*this).try_as_readwrite();
}

// -- raw_view (the erased variant) -> typed leaf, in a single call. `as_*` assert the variant holds the
//    matching resource arm (the arm then asserts the access class); `try_as_*` return nullopt when the arm is
//    a different resource kind or the access does not match. `T` / `Traits` are caller-supplied. --

template <view_element T>
[[nodiscard]] readonly_buffer_view<T> as_readonly_buffer(raw_view const& v)
{
    auto const* a = std::get_if<raw_buffer_view>(&v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a buffer arm");
    return a->as_readonly<T>();
}
template <view_element T>
[[nodiscard]] readwrite_buffer_view<T> as_readwrite_buffer(raw_view const& v)
{
    auto const* a = std::get_if<raw_buffer_view>(&v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a buffer arm");
    return a->as_readwrite<T>();
}
template <uniform_element T>
[[nodiscard]] uniform_buffer_view<T> as_uniform_buffer(raw_view const& v)
{
    auto const* a = std::get_if<raw_buffer_view>(&v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a buffer arm");
    return a->as_uniform<T>();
}
template <view_element T>
[[nodiscard]] cc::optional<readonly_buffer_view<T>> try_as_readonly_buffer(raw_view const& v)
{
    if (auto const* a = std::get_if<raw_buffer_view>(&v))
        return a->try_as_readonly<T>();
    return {};
}
template <view_element T>
[[nodiscard]] cc::optional<readwrite_buffer_view<T>> try_as_readwrite_buffer(raw_view const& v)
{
    if (auto const* a = std::get_if<raw_buffer_view>(&v))
        return a->try_as_readwrite<T>();
    return {};
}
template <uniform_element T>
[[nodiscard]] cc::optional<uniform_buffer_view<T>> try_as_uniform_buffer(raw_view const& v)
{
    if (auto const* a = std::get_if<raw_buffer_view>(&v))
        return a->try_as_uniform<T>();
    return {};
}

template <class Traits>
[[nodiscard]] readonly_texture_view<Traits> as_readonly_texture(raw_view const& v)
{
    auto const* a = std::get_if<raw_texture_view>(&v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a texture arm");
    return a->as_readonly<Traits>();
}
template <class Traits>
    requires storage_view_dimension<Traits::dimension>
[[nodiscard]] readwrite_texture_view<Traits> as_readwrite_texture(raw_view const& v)
{
    auto const* a = std::get_if<raw_texture_view>(&v);
    CC_ASSERT(a != nullptr, "raw_view does not hold a texture arm");
    return a->as_readwrite<Traits>();
}
template <class Traits>
[[nodiscard]] cc::optional<readonly_texture_view<Traits>> try_as_readonly_texture(raw_view const& v)
{
    if (auto const* a = std::get_if<raw_texture_view>(&v))
        return a->try_as_readonly<Traits>();
    return {};
}
template <class Traits>
    requires storage_view_dimension<Traits::dimension>
[[nodiscard]] cc::optional<readwrite_texture_view<Traits>> try_as_readwrite_texture(raw_view const& v)
{
    if (auto const* a = std::get_if<raw_texture_view>(&v))
        return a->try_as_readwrite<Traits>();
    return {};
}
} // namespace sg
