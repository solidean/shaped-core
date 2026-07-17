#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::offset_size
#include <clean-core/error/optional.hh> // cc::optional (try_reinterpret_as)
#include <shaped-graphics/raw_buffer.hh>

#include <type_traits> // std::is_same_v (view factories are pinned to the buffer's own element type)

namespace sg
{
/// A strongly-typed view onto a raw_buffer whose element type is fixed at compile time by `T` — think a
/// GPU-side `span<T>`. Privately holds a raw_buffer_handle; the view factories (`as_readonly_buffer()`,
/// `as_uniform_buffer()`, …) drop the element-type argument the raw API needs and infer it from `T`, and
/// are `requires`-gated so a nonsensical one (a uniform block of `byte`, a storage view of a non-DWORD
/// type) is a compile error. Reach the raw resource for the general (raw / byte-addressed) API via `raw()`.
/// Value type: copy is a cheap handle copy. Prefer creating one with `ctx.{persistent,transient}.create_buffer<T>(count, usage)`.
///
/// `T` must be trivially copyable (a buffer is raw GPU bytes) but is otherwise open — a vertex struct, an
/// index type, a uniform block, `byte`, …; each view factory imposes its own further constraint. Element
/// counts / ranges are in units of `T`.
template <class T>
class buffer
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "buffer<T>: the element type must be trivially copyable (raw GPU bytes)");

public:
    /// The element type this buffer is typed by, surfaced for call sites and introspection.
    using element_type = T;

    buffer() = default;

    // -- Wrapping a raw_buffer over the (byte size vs sizeof(T)) relationship: `from_raw` requires the byte
    //    size to be a whole number of `T` (asserts; `try_from_raw` is the checked twin), while
    //    `from_raw_clamped` tolerates a trailing partial element (element_count() floors). All require a
    //    non-null handle (null is a contract bug, not a runtime failure).

    /// Wrap a raw_buffer whose byte size is an exact multiple of `sizeof(T)` (a whole number of elements).
    [[nodiscard]] static buffer from_raw(raw_buffer_handle raw)
    {
        CC_ASSERT(raw != nullptr, "buffer<T> wraps a non-null raw_buffer");
        CC_ASSERT(raw->size_in_bytes() % isize(sizeof(T)) == 0, "buffer<T>::from_raw: byte size is not a whole number "
                                                                "of T");
        return buffer(cc::move(raw));
    }

    /// Checked from_raw — nullopt when the byte size is not a whole number of `T`.
    [[nodiscard]] static cc::optional<buffer> try_from_raw(raw_buffer_handle raw)
    {
        CC_ASSERT(raw != nullptr, "buffer<T> wraps a non-null raw_buffer");
        if (raw->size_in_bytes() % isize(sizeof(T)) != 0)
            return {};
        return buffer(cc::move(raw));
    }

    /// Wrap a raw_buffer, flooring to whole elements: a trailing partial element (byte size not a multiple
    /// of `sizeof(T)`) is simply ignored by element_count().
    [[nodiscard]] static buffer from_raw_clamped(raw_buffer_handle raw)
    {
        CC_ASSERT(raw != nullptr, "buffer<T> wraps a non-null raw_buffer");
        return buffer(cc::move(raw));
    }

    /// The underlying raw resource (never null unless default-constructed). Use this to reach the raw
    /// (general / byte-addressed) API; there is no implicit conversion to raw_buffer_handle.
    [[nodiscard]] raw_buffer_handle const& raw() const { return _raw; }

    [[nodiscard]] isize size_in_bytes() const { return _raw->size_in_bytes(); }
    [[nodiscard]] buffer_usage usage() const { return _raw->usage(); }

    /// Number of whole `T` elements the buffer holds (byte size / sizeof(T), truncating).
    [[nodiscard]] isize element_count() const { return _raw->size_in_bytes() / isize(sizeof(T)); }

    // -- Reinterpretation — view the same raw_buffer's storage as `buffer<U>`. Mirrors cc::span: the
    //    compile-time-checked `reinterpret_as` needs `sizeof(T)` divisible by `sizeof(U)` (U tiles T — e.g.
    //    buffer<vec3f> -> buffer<float>), so on an exact-fit buffer it is always clean; `try_reinterpret_as`
    //    handles the general case (U larger / unrelated) with a runtime size check.

    /// Reinterpret the storage as `buffer<U>`. `sizeof(T)` must be divisible by `sizeof(U)` (U tiles T), so
    /// on an exact-fit buffer the result is always exact; for the general case use try_reinterpret_as. (`U`
    /// is trivially copyable — enforced by `buffer<U>`.)
    template <class U>
    [[nodiscard]] buffer<U> reinterpret_as() const
    {
        static_assert(sizeof(T) % sizeof(U) == 0, "reinterpret_as needs sizeof(T) divisible by sizeof(U); use "
                                                  "try_reinterpret_as");
        CC_ASSERT(_raw != nullptr, "reinterpret_as on a null buffer");
        return buffer<U>::from_raw(_raw);
    }

    /// Like reinterpret_as but for any `U`: nullopt when the byte size is not a whole number of `U`.
    template <class U>
    [[nodiscard]] cc::optional<buffer<U>> try_reinterpret_as() const
    {
        CC_ASSERT(_raw != nullptr, "try_reinterpret_as on a null buffer");
        if (_raw->size_in_bytes() % isize(sizeof(U)) != 0)
            return {};
        return buffer<U>::from_raw(_raw);
    }

    // Shader-facing views, always in the buffer's own element type `T` (no reinterpretation). Each builds the
    // typed view directly and asserts the matching buffer_usage; ranges are in elements of `T`. (raw_buffer
    // itself carries only the byte-level as_raw_* factories — the typed path lives here, on buffer<T>.)
    //
    // The `template <class U = T> requires std::is_same_v<U, T>` shape is a deferral trick, not a
    // reinterpretation hook: the constrained view return type (readonly_buffer_view<T> requires view_element<T>,
    // uniform_buffer_view<T> requires uniform_element<T>) must not be *formed* until the view is actually used —
    // otherwise a buffer<u16> (a valid index buffer) would be ill-formed just for naming readonly_buffer_view<u16>,
    // whose element constraint u16 fails. Making the member a template defers that; pinning U == T keeps it
    // T-only. Use reinterpret_as for an explicit element-type change.

    /// Binds one element of the buffer as a uniform block (constant buffer / UBO) — `element_index` picks
    /// which (default: the first). A uniform_buffer_view is a single block, not a range. The element's byte offset
    /// (element_index * sizeof(T)) must be 256-byte aligned, so addressing past the first element needs `T`
    /// sized to a multiple of 256; drop to `raw()` for an arbitrary byte offset. Only where `T` obeys the
    /// uniform block rules. Requires uniform_buffer usage.
    template <class U = T>
    [[nodiscard]] uniform_buffer_view<U> as_uniform_buffer(isize element_index = 0) const
        requires(std::is_same_v<U, T> && uniform_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::uniform_buffer), "buffer lacks uniform_buffer usage");
        auto const offset = element_index * isize(sizeof(U));
        CC_ASSERT(offset % uniform_buffer_offset_alignment == 0, "uniform block offset must be 256-byte aligned");
        CC_ASSERT(offset >= 0 && offset + isize(sizeof(U)) <= _raw->size_in_bytes(), "uniform block does not fit in "
                                                                                     "buffer");
        return uniform_buffer_view<U>{.buffer = _raw, .offset_in_bytes = offset, .size_in_bytes = isize(sizeof(U))};
    }

    // A storage view is a *subrange* of the buffer, so unlike buffer<T> itself it carries the portable
    // binding rules: a 256-byte-aligned byte offset and a 4-byte-multiple byte size (see
    // storage_buffer_offset_alignment). A whole-buffer view starts at 0, so only the ranged overloads can
    // trip the offset rule — bind the whole buffer and index in the shader if you hit it.

    /// A read-only storage view of the whole buffer as an array of `T` (SRV). Requires readonly_buffer usage.
    template <class U = T>
    [[nodiscard]] readonly_buffer_view<U> as_readonly_buffer() const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        _assert_storage_placement(0, element_count() * isize(sizeof(T)));
        return readonly_buffer_view<U>{.buffer = _raw, .offset_in_bytes = 0, .element_count = element_count()};
    }

    /// A read-only storage view of `range` elements of `T` (SRV). Requires readonly_buffer usage.
    template <class U = T>
    [[nodiscard]] readonly_buffer_view<U> as_readonly_buffer(cc::offset_size range) const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        auto const offset = _element_offset(range);
        _assert_storage_placement(offset, range.size * isize(sizeof(T)));
        return readonly_buffer_view<U>{.buffer = _raw, .offset_in_bytes = offset, .element_count = range.size};
    }

    /// A read-write storage view of the whole buffer as an array of `T` (UAV). Requires readwrite_buffer usage.
    template <class U = T>
    [[nodiscard]] readwrite_buffer_view<U> as_readwrite_buffer() const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        _assert_storage_placement(0, element_count() * isize(sizeof(T)));
        return readwrite_buffer_view<U>{.buffer = _raw, .offset_in_bytes = 0, .element_count = element_count()};
    }

    /// A read-write storage view of `range` elements of `T` (UAV). Requires readwrite_buffer usage.
    template <class U = T>
    [[nodiscard]] readwrite_buffer_view<U> as_readwrite_buffer(cc::offset_size range) const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        auto const offset = _element_offset(range);
        _assert_storage_placement(offset, range.size * isize(sizeof(T)));
        return readwrite_buffer_view<U>{.buffer = _raw, .offset_in_bytes = offset, .element_count = range.size};
    }

    // Checked twins of the storage / uniform view factories: nullopt when the range breaks the placement rules
    // (bounds, 256-byte offset, 4-byte size, uniform block rules) — the conditions a caller computing an offset
    // at runtime can genuinely hit. A missing buffer_usage flag still asserts: you chose the usage at creation.

    /// Checked as_readonly_buffer (whole buffer) — nullopt when the size breaks the storage rules.
    template <class U = T>
    [[nodiscard]] cc::optional<readonly_buffer_view<U>> try_as_readonly_buffer() const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        if (!_is_valid_storage_placement(0, element_count() * isize(sizeof(T))))
            return {};
        return as_readonly_buffer();
    }

    /// Checked as_readonly_buffer — nullopt when `range` breaks the bounds / storage rules.
    template <class U = T>
    [[nodiscard]] cc::optional<readonly_buffer_view<U>> try_as_readonly_buffer(cc::offset_size range) const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        if (!_is_valid_element_range(range)
            || !_is_valid_storage_placement(range.offset * isize(sizeof(T)), range.size * isize(sizeof(T))))
            return {};
        return as_readonly_buffer(range);
    }

    /// Checked as_readwrite_buffer (whole buffer) — nullopt when the size breaks the storage rules.
    template <class U = T>
    [[nodiscard]] cc::optional<readwrite_buffer_view<U>> try_as_readwrite_buffer() const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        if (!_is_valid_storage_placement(0, element_count() * isize(sizeof(T))))
            return {};
        return as_readwrite_buffer();
    }

    /// Checked as_readwrite_buffer — nullopt when `range` breaks the bounds / storage rules.
    template <class U = T>
    [[nodiscard]] cc::optional<readwrite_buffer_view<U>> try_as_readwrite_buffer(cc::offset_size range) const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        if (!_is_valid_element_range(range)
            || !_is_valid_storage_placement(range.offset * isize(sizeof(T)), range.size * isize(sizeof(T))))
            return {};
        return as_readwrite_buffer(range);
    }

    /// Checked as_uniform_buffer — nullopt when the block's byte offset is not 256-byte aligned or does not fit.
    template <class U = T>
    [[nodiscard]] cc::optional<uniform_buffer_view<U>> try_as_uniform_buffer(isize element_index = 0) const
        requires(std::is_same_v<U, T> && uniform_element<U>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::uniform_buffer), "buffer lacks uniform_buffer usage");
        auto const offset = element_index * isize(sizeof(U));
        if (offset % uniform_buffer_offset_alignment != 0)
            return {};
        if (offset < 0 || offset + isize(sizeof(U)) > _raw->size_in_bytes())
            return {};
        return as_uniform_buffer(element_index);
    }

    // Draw-input views (bound at draw time, not shader-visible). Ranges are in elements of `T`.
    // NOTE: as_vertex_buffer only conveys the per-vertex stride (sizeof(T)) today — it does not yet tie `T`
    // to the pipeline's vertex_input_layout (attribute offsets / formats). A `vertex_layout<T>` trait that
    // lets binding cross-check the layout (and build it from `T`) is a deferred extension, landing with the
    // raster-pipeline vertex-input work.
    // TODO: validate a bound vertex buffer's element type against the vertex_input_layout the pipeline
    // expects. Carrying a std::type_info / std::type_index of `T` on the view (recorded via typeid(T)) and
    // the expected type on the layout would let binding cross-check them at runtime, even before the
    // compile-time vertex_layout<T> trait lands.

    /// The whole buffer as a vertex buffer with per-vertex stride `sizeof(T)`. Requires vertex_buffer usage.
    [[nodiscard]] vertex_buffer_view as_vertex_buffer() const
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::vertex_buffer), "buffer lacks vertex_buffer usage");
        return vertex_buffer_view{.buffer = _raw,
                                  .offset_in_bytes = 0,
                                  .size_in_bytes = _raw->size_in_bytes(),
                                  .stride_in_bytes = isize(sizeof(T))};
    }

    /// A vertex buffer over `range` vertices of `T` (stride `sizeof(T)`). Requires vertex_buffer usage.
    [[nodiscard]] vertex_buffer_view as_vertex_buffer(cc::offset_size range) const
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::vertex_buffer), "buffer lacks vertex_buffer usage");
        return vertex_buffer_view{.buffer = _raw,
                                  .offset_in_bytes = _element_offset(range),
                                  .size_in_bytes = range.size * isize(sizeof(T)),
                                  .stride_in_bytes = isize(sizeof(T))};
    }

    /// The whole buffer as an index buffer; the index width follows `T` (u16 → uint16, u32 → uint32).
    /// Only on 16-/32-bit unsigned element types. Requires index_buffer usage.
    [[nodiscard]] index_buffer_view as_index_buffer() const
        requires(std::is_same_v<T, u16> || std::is_same_v<T, u32>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::index_buffer), "buffer lacks index_buffer usage");
        return index_buffer_view{.buffer = _raw,
                                 .format = _index_format(),
                                 .offset_in_bytes = 0,
                                 .size_in_bytes = _raw->size_in_bytes()};
    }

    /// An index buffer over `range` indices of `T`. Only on 16-/32-bit unsigned element types. Requires index_buffer usage.
    [[nodiscard]] index_buffer_view as_index_buffer(cc::offset_size range) const
        requires(std::is_same_v<T, u16> || std::is_same_v<T, u32>)
    {
        CC_ASSERT(has_flag(_raw->usage(), buffer_usage::index_buffer), "buffer lacks index_buffer usage");
        return index_buffer_view{.buffer = _raw,
                                 .format = _index_format(),
                                 .offset_in_bytes = _element_offset(range),
                                 .size_in_bytes = range.size * isize(sizeof(T))};
    }

private:
    // Non-null / fit are checked by the `from_raw` / `from_raw_clamped` factories before this runs.
    explicit buffer(raw_buffer_handle raw) : _raw(cc::move(raw)) {}

    // Whether an element range fits the buffer. Non-asserting twin of the bounds check in _element_offset.
    [[nodiscard]] bool _is_valid_element_range(cc::offset_size range) const
    {
        return range.offset >= 0 && range.size >= 0
            && (range.offset + range.size) * isize(sizeof(T)) <= _raw->size_in_bytes();
    }

    // Whether a byte range obeys the storage placement rules. Non-asserting twin of _assert_storage_placement.
    [[nodiscard]] static bool _is_valid_storage_placement(isize offset_in_bytes, isize size_in_bytes)
    {
        return offset_in_bytes % storage_buffer_offset_alignment == 0
            && size_in_bytes % storage_buffer_size_alignment == 0;
    }

    // The portable placement rules for a shader-facing storage view's byte range (see
    // storage_buffer_offset_alignment). Only the views carry these — buffer<T> itself is a whole buffer and
    // recasts like a span, so from_raw / reinterpret_as / the draw-input (vertex, index) views are exempt;
    // that keeps e.g. a buffer<u16> index buffer perfectly legal.
    void _assert_storage_placement(isize offset_in_bytes, isize size_in_bytes) const
    {
        CC_ASSERT(offset_in_bytes % storage_buffer_offset_alignment == 0, "storage view offset must be 256-byte "
                                                                          "aligned (WebGPU; some Vulkan hardware) — "
                                                                          "bind the whole buffer and index in-shader");
        CC_ASSERT(size_in_bytes % storage_buffer_size_alignment == 0, "storage view size must be a multiple of 4 bytes "
                                                                      "(WebGPU)");
    }

    // The byte offset of element-range `range` (in elements of T), bounds-checked against the buffer size.
    [[nodiscard]] isize _element_offset(cc::offset_size range) const
    {
        auto const stride = isize(sizeof(T));
        CC_ASSERT(range.offset >= 0 && range.size >= 0, "view range must be non-negative");
        CC_ASSERT((range.offset + range.size) * stride <= _raw->size_in_bytes(), "view range exceeds buffer size");
        return range.offset * stride;
    }

    // The index width for T (only instantiated from the u16/u32-gated as_index_buffer overloads).
    [[nodiscard]] static constexpr index_format _index_format()
    {
        return std::is_same_v<T, u16> ? index_format::uint16 : index_format::uint32;
    }

    raw_buffer_handle _raw = nullptr;
};

// raw_buffer -> buffer<T> (declared on raw_buffer, defined here where buffer<T> is complete). Thin wrappers
// over from_raw / try_from_raw, so a handle re-types with `raw->as_buffer<T>()` instead of naming buffer<T>.
template <class T>
auto raw_buffer::as_buffer() const
{
    return buffer<T>::from_raw(shared_from_this());
}
template <class T>
auto raw_buffer::try_as_buffer() const
{
    return buffer<T>::try_from_raw(shared_from_this());
}
} // namespace sg
