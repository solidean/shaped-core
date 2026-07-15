#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::offset_size
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
/// `T` is unconstrained at the class level (it may be a vertex struct, an index type, a uniform block, or
/// any trivially-copyable GPU type); each view factory imposes its own constraint. Element counts / ranges
/// are in units of `T`.
template <class T>
class buffer
{
public:
    /// The element type this buffer is typed by, surfaced for call sites and introspection.
    using element_type = T;

    buffer() = default;

    /// Wraps a raw_buffer. The handle must be non-null. The buffer's byte size need not be an exact
    /// multiple of `sizeof(T)` — element_count() truncates, matching the raw view API.
    explicit buffer(raw_buffer_handle raw) : _raw(cc::move(raw))
    {
        CC_ASSERT(_raw != nullptr, "buffer<T> wraps a non-null raw_buffer");
    }

    /// The underlying raw resource (never null unless default-constructed). Use this to reach the raw
    /// (general / byte-addressed) API; there is no implicit conversion to raw_buffer_handle.
    [[nodiscard]] raw_buffer_handle const& raw() const { return _raw; }

    [[nodiscard]] isize size_in_bytes() const { return _raw->size_in_bytes(); }
    [[nodiscard]] buffer_usage usage() const { return _raw->usage(); }

    /// Number of whole `T` elements the buffer holds (byte size / sizeof(T), truncating).
    [[nodiscard]] isize element_count() const { return _raw->size_in_bytes() / isize(sizeof(T)); }

    // Shader-facing views, always in the buffer's own element type `T` (no reinterpretation). Each delegates
    // to the raw_buffer view factory and asserts the matching buffer_usage; ranges are in elements of `T`.
    //
    // The `template <class U = T> requires std::is_same_v<U, T>` shape is a deferral trick, not a
    // reinterpretation hook: the constrained view return type (readonly_view<T> requires view_element<T>,
    // uniform_view<T> requires uniform_element<T>) must not be *formed* until the view is actually used —
    // otherwise a buffer<u16> (a valid index buffer) would be ill-formed just for naming readonly_view<u16>,
    // whose element constraint u16 fails. Making the member a template defers that; pinning U == T keeps it
    // T-only. If explicit reinterpretation is ever wanted, it gets its own named API.

    /// Binds one element of the buffer as a uniform block (constant buffer / UBO) — `element_index` picks
    /// which (default: the first). A uniform_view is a single block, not a range. The element's byte offset
    /// (element_index * sizeof(T)) must be 256-byte aligned, so addressing past the first element needs `T`
    /// sized to a multiple of 256; drop to `raw()` for an arbitrary byte offset. Only where `T` obeys the
    /// uniform block rules. Requires uniform_buffer usage.
    template <class U = T>
    [[nodiscard]] uniform_view<U> as_uniform_buffer(isize element_index = 0) const
        requires(std::is_same_v<U, T> && uniform_element<U>)
    {
        return _raw->template as_uniform_buffer<U>(element_index * isize(sizeof(U)));
    }

    /// A read-only storage view of the whole buffer as an array of `T` (SRV). Requires readonly_buffer usage.
    template <class U = T>
    [[nodiscard]] readonly_view<U> as_readonly_buffer() const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        return _raw->template as_readonly_buffer<U>();
    }

    /// A read-only storage view of `range` elements of `T` (SRV). Requires readonly_buffer usage.
    template <class U = T>
    [[nodiscard]] readonly_view<U> as_readonly_buffer(cc::offset_size range) const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        return _raw->template as_readonly_buffer<U>(range);
    }

    /// A read-write storage view of the whole buffer as an array of `T` (UAV). Requires readwrite_buffer usage.
    template <class U = T>
    [[nodiscard]] readwrite_view<U> as_readwrite_buffer() const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        return _raw->template as_readwrite_buffer<U>();
    }

    /// A read-write storage view of `range` elements of `T` (UAV). Requires readwrite_buffer usage.
    template <class U = T>
    [[nodiscard]] readwrite_view<U> as_readwrite_buffer(cc::offset_size range) const
        requires(std::is_same_v<U, T> && view_element<U>)
    {
        return _raw->template as_readwrite_buffer<U>(range);
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
    [[nodiscard]] vertex_buffer_view as_vertex_buffer() const { return _raw->template as_vertex_buffer<T>(); }

    /// A vertex buffer over `range` vertices of `T` (stride `sizeof(T)`). Requires vertex_buffer usage.
    [[nodiscard]] vertex_buffer_view as_vertex_buffer(cc::offset_size range) const
    {
        return _raw->template as_vertex_buffer<T>(range);
    }

    /// The whole buffer as an index buffer; the index width follows `T` (u16 → uint16, u32 → uint32).
    /// Only on 16-/32-bit unsigned element types. Requires index_buffer usage.
    [[nodiscard]] index_buffer_view as_index_buffer() const
        requires(std::is_same_v<T, u16> || std::is_same_v<T, u32>)
    {
        return _raw->as_index_buffer(_index_format());
    }

    /// An index buffer over `range` indices of `T`. Only on 16-/32-bit unsigned element types. Requires index_buffer usage.
    [[nodiscard]] index_buffer_view as_index_buffer(cc::offset_size range) const
        requires(std::is_same_v<T, u16> || std::is_same_v<T, u32>)
    {
        return _raw->as_index_buffer(_index_format(), range);
    }

private:
    // The index width for T (only instantiated from the u16/u32-gated as_index_buffer overloads).
    [[nodiscard]] static constexpr index_format _index_format()
    {
        return std::is_same_v<T, u16> ? index_format::uint16 : index_format::uint32;
    }

    raw_buffer_handle _raw = nullptr;
};
} // namespace sg
