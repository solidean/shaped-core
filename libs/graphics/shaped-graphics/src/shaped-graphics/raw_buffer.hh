#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>
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

    // Typed views onto this buffer — a strongly-typed handle a rendering routine binds. Each returns
    // the view for its access class (uniform / readonly / readwrite), typed by the element/block type
    // `T` (`byte` for a raw, byte-addressed view). Ranges are in elements of `T`; the default is the
    // whole buffer. The buffer's usage must cover the view's access. See views.hh.

    /// A uniform block of `T` (constant buffer / UBO) at `offset_in_bytes` (default: the buffer start —
    /// pass an offset to select one block of a UBO array). The offset must be 256-byte aligned; `T`'s
    /// size rules (multiple of 16, <= 64 KiB) are enforced by uniform_element. Requires uniform_buffer usage.
    template <uniform_element T>
    [[nodiscard]] uniform_view<T> as_uniform_buffer(isize offset_in_bytes = 0) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::uniform_buffer), "buffer lacks uniform_buffer usage");
        CC_ASSERT(offset_in_bytes % uniform_buffer_offset_alignment == 0, "uniform block offset must be 256-byte "
                                                                          "aligned");
        CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + isize(sizeof(T)) <= _size_in_bytes, "uniform block does "
                                                                                                "not fit in buffer");
        return uniform_view<T>{.buffer = shared_from_this(),
                               .offset_in_bytes = offset_in_bytes,
                               .size_in_bytes = isize(sizeof(T))};
    }

    /// A read-only storage view of the whole buffer as an array of `T` (SRV). Requires readonly_buffer usage.
    template <view_element T>
    [[nodiscard]] readonly_view<T> as_readonly_buffer() const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        return readonly_view<T>{.buffer = shared_from_this(),
                                .offset_in_bytes = 0,
                                .element_count = _size_in_bytes / isize(sizeof(T))};
    }

    /// A read-only storage view of `range` elements of `T` (SRV). Requires readonly_buffer usage.
    template <view_element T>
    [[nodiscard]] readonly_view<T> as_readonly_buffer(cc::offset_size range) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readonly_buffer), "buffer lacks readonly_buffer usage");
        return readonly_view<T>{.buffer = shared_from_this(),
                                .offset_in_bytes = element_offset<T>(range),
                                .element_count = range.size};
    }

    /// A read-write storage view of the whole buffer as an array of `T` (UAV). Requires readwrite_buffer usage.
    template <view_element T>
    [[nodiscard]] readwrite_view<T> as_readwrite_buffer() const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        return readwrite_view<T>{.buffer = shared_from_this(),
                                 .offset_in_bytes = 0,
                                 .element_count = _size_in_bytes / isize(sizeof(T))};
    }

    /// A read-write storage view of `range` elements of `T` (UAV). Requires readwrite_buffer usage.
    template <view_element T>
    [[nodiscard]] readwrite_view<T> as_readwrite_buffer(cc::offset_size range) const
    {
        CC_ASSERT(has_flag(_usage, buffer_usage::readwrite_buffer), "buffer lacks readwrite_buffer usage");
        return readwrite_view<T>{.buffer = shared_from_this(),
                                 .offset_in_bytes = element_offset<T>(range),
                                 .element_count = range.size};
    }

    /// A raw, byte-addressed read-only view (SRV over `byte`) of the whole buffer. Requires readonly_buffer usage.
    [[nodiscard]] readonly_view<cc::byte> as_raw_readonly() const { return as_readonly_buffer<cc::byte>(); }

    /// A raw, byte-addressed read-only view of `range` bytes (SRV over `byte`). Requires readonly_buffer usage.
    [[nodiscard]] readonly_view<cc::byte> as_raw_readonly(cc::offset_size range) const
    {
        return as_readonly_buffer<cc::byte>(range);
    }

    /// A raw, byte-addressed read-write view (UAV over `byte`) of the whole buffer. Requires readwrite_buffer usage.
    [[nodiscard]] readwrite_view<cc::byte> as_raw_readwrite() const { return as_readwrite_buffer<cc::byte>(); }

    /// A raw, byte-addressed read-write view of `range` bytes (UAV over `byte`). Requires readwrite_buffer usage.
    [[nodiscard]] readwrite_view<cc::byte> as_raw_readwrite(cc::offset_size range) const
    {
        return as_readwrite_buffer<cc::byte>(range);
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

    /// Validates a `range` given in elements of `T` against the buffer bounds and returns its byte offset.
    template <view_element T>
    [[nodiscard]] isize element_offset(cc::offset_size range) const
    {
        auto const stride = isize(sizeof(T));
        CC_ASSERT(range.offset >= 0 && range.size >= 0, "view range must be non-negative");
        CC_ASSERT((range.offset + range.size) * stride <= _size_in_bytes, "view range exceeds buffer size");
        return range.offset * stride;
    }

    isize _size_in_bytes = 0;
    buffer_usage _usage = buffer_usage::none;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired{false};                   // mutable: expire() is a const lifetime hook
};
} // namespace sg
