#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/index_buffer_view.hh>
#include <shaped-graphics/types.hh>
#include <shaped-graphics/vertex_buffer_view.hh>
#include <shaped-graphics/views.hh>

#include <atomic>
#include <memory>

namespace sg
{
/// A GPU-resident buffer with immutable shape (size + usage) — like a span over mutable GPU memory:
/// contents change through command lists, but it can't be resized or repurposed. No host-visible
/// mapping (transfers go through command lists). Size 0 is a valid empty buffer. Held via raw_buffer_handle.
///
/// Abstract: a backend subclasses it and owns the GPU resource. Shape metadata lives here as
/// protected members that backends read and set directly.
class raw_buffer : public std::enable_shared_from_this<raw_buffer>
{
public:
    virtual ~raw_buffer();

    /// Size of the buffer's GPU storage in bytes.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// How the buffer may be used (copy source/dest, vertex, index, ...).
    [[nodiscard]] buffer_usage usage() const { return _usage; }

    // Byte-level views onto this buffer — the low-level path. Every factory here works in *bytes* (a
    // cc::offset_size byte range, an explicit stride), with no C++ element type: as_raw_readonly /
    // as_raw_readwrite (byte-addressed, or structured via an explicit stride), as_raw_uniform_buffer,
    // as_raw_vertex_buffer, and as_index_buffer / as_raw_index_buffer. The buffer's usage must cover the access.
    //
    // For the ergonomic, element-typed path (as_readonly_buffer(), as_uniform_buffer(), … inferring `T`)
    // wrap the buffer in a `buffer<T>` (buffer.hh) — the friction here is deliberate.

    /// Raw uniform (constant buffer) view over an explicit byte range. The offset must be 256-byte aligned,
    /// the range must fit, and its size must be <= 64 KiB (max CBV). Returns the erased arm (there is no
    /// untyped uniform view). Requires uniform_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_uniform_buffer(cc::offset_size byte_range) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::uniform_buffer), "buffer lacks uniform_buffer usage");
        CC_ASSERT(byte_range.offset % uniform_buffer_offset_alignment == 0, "uniform block offset must be 256-byte "
                                                                            "aligned");
        CC_ASSERT(byte_range.offset >= 0 && byte_range.size >= 0, "view range must be non-negative");
        CC_ASSERT(byte_range.offset + byte_range.size <= _size_in_bytes, "uniform range exceeds buffer size");
        CC_ASSERT(byte_range.size <= max_uniform_buffer_size, "uniform range exceeds the 64 KiB max CBV size");
        return raw_buffer_view{.access = view_class::uniform,
                               .shape = view_shape::uniform_block,
                               .buffer = shared_from_this(),
                               .offset_in_bytes = byte_range.offset,
                               .size_in_bytes = byte_range.size};
    }

    /// A raw, byte-addressed read-only view (SRV, `shape == raw`) of the whole buffer. Requires readonly_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_readonly() const
    {
        return as_raw_readonly({.offset = 0, .size = _size_in_bytes});
    }

    /// A raw, byte-addressed read-only view of `byte_range` bytes (SRV, `shape == raw`). Requires readonly_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_readonly(cc::offset_size byte_range) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        assert_byte_range(byte_range);
        return raw_buffer_view{.access = view_class::readonly,
                               .shape = view_shape::raw,
                               .buffer = shared_from_this(),
                               .offset_in_bytes = byte_range.offset,
                               .size_in_bytes = byte_range.size};
    }

    /// A raw *structured* read-only view (SRV, `shape == structured`) over `byte_range` bytes with an explicit
    /// element `stride_in_bytes`; element_count = byte_range.size / stride. Requires readonly_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_readonly(cc::offset_size byte_range, isize stride_in_bytes) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        assert_structured_range(byte_range, stride_in_bytes);
        return raw_buffer_view{.access = view_class::readonly,
                               .shape = view_shape::structured,
                               .buffer = shared_from_this(),
                               .offset_in_bytes = byte_range.offset,
                               .element_count = byte_range.size / stride_in_bytes,
                               .stride_in_bytes = stride_in_bytes};
    }

    /// A raw, byte-addressed read-write view (UAV, `shape == raw`) of the whole buffer. Requires readwrite_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_readwrite() const
    {
        return as_raw_readwrite({.offset = 0, .size = _size_in_bytes});
    }

    /// A raw, byte-addressed read-write view of `byte_range` bytes (UAV, `shape == raw`). Requires readwrite_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_readwrite(cc::offset_size byte_range) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        assert_byte_range(byte_range);
        return raw_buffer_view{.access = view_class::readwrite,
                               .shape = view_shape::raw,
                               .buffer = shared_from_this(),
                               .offset_in_bytes = byte_range.offset,
                               .size_in_bytes = byte_range.size};
    }

    /// A raw *structured* read-write view (UAV, `shape == structured`) over `byte_range` bytes with an explicit
    /// element `stride_in_bytes`; element_count = byte_range.size / stride. Requires readwrite_buffer usage.
    [[nodiscard]] raw_buffer_view as_raw_readwrite(cc::offset_size byte_range, isize stride_in_bytes) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        assert_structured_range(byte_range, stride_in_bytes);
        return raw_buffer_view{.access = view_class::readwrite,
                               .shape = view_shape::structured,
                               .buffer = shared_from_this(),
                               .offset_in_bytes = byte_range.offset,
                               .element_count = byte_range.size / stride_in_bytes,
                               .stride_in_bytes = stride_in_bytes};
    }

    // Attachment-less draw-input views — a vertex or index buffer bound at draw time (not shader-visible,
    // so they don't go through views.hh / binding groups). Bind via cmd.raster.bind_vertex_buffers /
    // bind_index_buffer.

    /// Raw vertex buffer view over an explicit byte range with an explicit per-vertex `stride_in_bytes`
    /// (which must match the pipeline's slot). Requires vertex_buffer usage.
    [[nodiscard]] vertex_buffer_view as_raw_vertex_buffer(cc::offset_size byte_range, isize stride_in_bytes) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::vertex_buffer), "buffer lacks vertex_buffer usage");
        CC_ASSERT(byte_range.offset >= 0 && byte_range.size >= 0 && stride_in_bytes >= 0, "view range / stride must be "
                                                                                          "non-negative");
        CC_ASSERT(byte_range.offset + byte_range.size <= _size_in_bytes, "view range exceeds buffer size");
        return vertex_buffer_view{.buffer = shared_from_this(),
                                  .offset_in_bytes = byte_range.offset,
                                  .size_in_bytes = byte_range.size,
                                  .stride_in_bytes = stride_in_bytes};
    }

    /// The whole buffer as an index buffer of `format` elements. Requires index_buffer usage.
    [[nodiscard]] index_buffer_view as_index_buffer(index_format format = index_format::uint16) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::index_buffer), "buffer lacks index_buffer usage");
        return index_buffer_view{.buffer = shared_from_this(),
                                 .format = format,
                                 .offset_in_bytes = 0,
                                 .size_in_bytes = _size_in_bytes};
    }

    /// Raw index buffer view over an explicit byte range (index width follows `format`). Requires index_buffer usage.
    [[nodiscard]] index_buffer_view as_raw_index_buffer(index_format format, cc::offset_size byte_range) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::index_buffer), "buffer lacks index_buffer usage");
        CC_ASSERT(byte_range.offset >= 0 && byte_range.size >= 0, "view range must be non-negative");
        CC_ASSERT(byte_range.offset + byte_range.size <= _size_in_bytes, "view range exceeds buffer size");
        return index_buffer_view{.buffer = shared_from_this(),
                                 .format = format,
                                 .offset_in_bytes = byte_range.offset,
                                 .size_in_bytes = byte_range.size};
    }

    /// Registers a callback to run once this buffer's GPU storage is released *and* no longer in
    /// flight (its owning epoch has retired). The feedback point for reclaiming externally-owned
    /// backing memory (e.g. placed resources on a custom allocator). Do not assume which thread runs it.
    /// Const: registering a finalizer is a lifetime hook, not a change to the buffer's shape.
    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }

    // Expiry — a buffer may be marked expired (its storage reclaimed) while handles to it still exist;
    // naming an expired buffer (in a transfer or a binding) is invalid. This is explicit state, not tied
    // to any one lifetime mode: a transient buffer is auto-expired when its epoch advances, and a
    // persistent buffer can be expired explicitly to free its storage early without dropping every handle.

    /// Whether this buffer's storage has been reclaimed. Once true, it never goes back to false.
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }

    /// The negation of is_expired(): the buffer still names live storage.
    [[nodiscard]] bool is_valid() const { return !is_expired(); }

    /// Expire the buffer now, releasing its GPU storage (deferred until no longer in flight). Idempotent.
    /// Const: expiry is a lifetime operation, not a change to the buffer's shape.
    void expire() const
    {
        if (!_expired.exchange(true, std::memory_order_acq_rel))
            on_expired();
    }

protected:
    raw_buffer(isize size_in_bytes, buffer_usage usage);

    /// Backend hook run once, from expire(), after the buffer is marked expired: release the GPU
    /// storage (backends defer it until the owning epoch retires). Default: nothing to release.
    virtual void on_expired() const {}

    /// Bounds-checks a byte range against the buffer size (non-negative, within bounds).
    void assert_byte_range(cc::offset_size byte_range) const
    {
        CC_ASSERT(byte_range.offset >= 0 && byte_range.size >= 0, "view range must be non-negative");
        CC_ASSERT(byte_range.offset + byte_range.size <= _size_in_bytes, "view range exceeds buffer size");
    }

    /// Bounds-checks a structured byte range: positive stride, in bounds, size a whole number of elements.
    void assert_structured_range(cc::offset_size byte_range, isize stride_in_bytes) const
    {
        CC_ASSERT(stride_in_bytes > 0, "structured view stride must be positive");
        assert_byte_range(byte_range);
        CC_ASSERT(byte_range.size % stride_in_bytes == 0, "structured view size must be a whole number of elements");
    }

    isize _size_in_bytes = 0;
    buffer_usage _usage = buffer_usage::none;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired{false};                   // mutable: expire() is a const lifetime hook
};
} // namespace sg
