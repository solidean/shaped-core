#pragma once

#include <shaped-graphics/fwd.hh>

#include <type_traits>

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

/// A `uniform_view` block type: a `view_element` whose size obeys the uniform block rules above (a
/// multiple of 16, at most 64 KiB). Excludes `byte` — a uniform block of raw bytes is meaningless.
template <class T>
concept uniform_element = view_element<T> && (sizeof(T) % 16 == 0) && (isize(sizeof(T)) <= max_uniform_buffer_size);

/// How a shader reads a view. Mirrors buffer_usage's uniform/readonly/readwrite split.
enum class view_class
{
    uniform,   ///< uniform block — constant buffer / UBO (read-only)
    readonly,  ///< read-only storage — SRV / read SSBO
    readwrite, ///< read-write storage — UAV / read-write SSBO
    // Future (with textures / samplers): render_target, depth_stencil, sampler, acceleration_structure.
};

/// How a view's bytes are laid out. `raw` is byte-addressed (element type `byte`); `structured` is an
/// array strided by the element type; `uniform_block` is a single struct block.
enum class view_shape
{
    uniform_block,
    structured,
    raw,
    // Future (with textures / formats): texel, texture_1d, texture_2d, texture_2d_array, texture_3d, ...
};

/// The erased form every typed view converts into — the value a backend reads (via `(access, shape)`)
/// to build a native descriptor. Fields not relevant to a given `(access, shape)` stay zero.
struct raw_view
{
    view_class access;
    view_shape shape;
    buffer_handle buffer;      ///< the viewed buffer
    isize offset_in_bytes = 0; ///< start of the view within the buffer
    isize size_in_bytes = 0;   ///< [uniform_block, raw] visible byte size
    isize element_count = 0;   ///< [structured] number of elements
    isize stride_in_bytes = 0; ///< [structured] element stride (= sizeof(T))
};

/// A uniform block of `T` — a constant buffer / UBO binding (read-only). {buffer, offset, sizeof(T)}.
template <uniform_element T>
struct uniform_view
{
    static constexpr view_class access = view_class::uniform;

    buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize size_in_bytes = isize(sizeof(T));

    [[nodiscard]] raw_view to_raw() const
    {
        return raw_view{
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
struct readonly_view
{
    static constexpr view_class access = view_class::readonly;

    buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize element_count = 0; ///< count of `T` (for `byte`, a count of bytes)

    [[nodiscard]] raw_view to_raw() const
    {
        constexpr bool is_raw = std::is_same_v<T, cc::byte>;
        return raw_view{
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
struct readwrite_view
{
    static constexpr view_class access = view_class::readwrite;

    buffer_handle buffer;
    isize offset_in_bytes = 0;
    isize element_count = 0; ///< count of `T` (for `byte`, a count of bytes)

    [[nodiscard]] raw_view to_raw() const
    {
        constexpr bool is_raw = std::is_same_v<T, cc::byte>;
        return raw_view{
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
} // namespace sg
