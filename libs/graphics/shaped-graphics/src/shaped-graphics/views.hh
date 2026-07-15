#pragma once

#include <clean-core/common/utility.hh>           // cc::move
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

/// A read-only (sampled / SRV) texture view over a subresource range. Unlike buffer views it is not
/// templated on an element type — the texel format is a runtime `pixel_format`, and dimension / array /
/// cube / samples come from the bound texture. Built via `texture<Traits>::as_readonly_view()`.
struct texture_readonly_view
{
    static constexpr view_class access = view_class::readonly;

    raw_texture_handle texture;
    texture_view_dimension dimension = texture_view_dimension::tex_2d;
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

/// A read-write (storage / UAV) texture view over a subresource range (a single mip level). Built via
/// `texture<Traits>::as_readwrite_view()` and friends.
struct texture_readwrite_view
{
    static constexpr view_class access = view_class::readwrite;

    raw_texture_handle texture;
    texture_view_dimension dimension = texture_view_dimension::tex_2d;
    pixel_format format = pixel_format::undefined;
    subresource_range range;

    /// For a 3D storage view (`dimension == tex_3d`): the half-open `[start, end)` window of depth slices
    /// (a 3D texture's W / Z axis — D3D12's `FirstWSlice`/`WSize`) the view exposes, in slices of the
    /// selected mip. These are *not* subresources (a whole 3D mip is one), so they live here, not in
    /// `range`. Empty `{0, 0}` for every non-3D view, which ignore it.
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
} // namespace sg
