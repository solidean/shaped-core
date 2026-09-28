#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh> // typed buffer<T> — the preferred overloads below take it
#include <shaped-graphics/resource/texture_region.hh>

#include <ranges>
#include <type_traits>

namespace sg
{
/// A contiguous range of trivially-copyable elements — what the `create_buffer_from_data` factories fill a buffer from.
/// A cc::pinned_data, a cc::vector, a cc::span and a C array all qualify.
template <class Range>
concept upload_range
    = std::ranges::contiguous_range<Range> && std::is_trivially_copyable_v<std::ranges::range_value_t<Range>>;

/// The byte-only upload_range `create_buffer_from_bytes` takes.
template <class Range>
concept upload_byte_range = upload_range<Range> && std::is_same_v<std::ranges::range_value_t<Range>, byte>;
} // namespace sg

namespace sg::impl
{
/// Pins `data` for ctx.upload as cheaply as cc::make_pinned_data can.
/// A pinned_data or an owning rvalue keeps its elements where they are; an lvalue, a borrow or a C array is copied once.
template <upload_range Range>
[[nodiscard]] auto pin_upload_range(Range&& data)
{
    if constexpr (std::is_array_v<std::remove_reference_t<Range>>)
        return cc::make_pinned_data(cc::span<std::remove_extent_t<std::remove_reference_t<Range>>>(data));
    else
        return cc::make_pinned_data(cc::forward<Range>(data));
}
} // namespace sg::impl

/// Async host→device upload facade for a context, reached as `ctx.upload`.
///
/// The context-level mirror of the inline `cmd.upload`, which records a copy inline in a command list, visible to later commands in that list.
/// `ctx.upload` instead streams the copy on a dedicated copy queue, off the frame path.
/// It is the right tool for bulk asset streaming, not small must-be-visible-now per-frame writes.
///
/// Fire-and-forget: the call returns immediately and the copy runs asynchronously.
/// The pin holds the source bytes alive until the copy has consumed them, so the caller may free the original right away — unlike inline upload, which copies synchronously.
/// A later command list that reads the buffer automatically waits on the copy, with no manual synchronization.
/// Uploads to one buffer keep their submission order; across buffers the order is unconstrained.
class sg::context_upload_scope
{
    // Typed-buffer overload — the preferred form.
    // `buffer<T>` alone names the element type, so a pin of any other element type is a compile error at the argument rather than a silently reinterpreted upload.
public:
    /// Streams a pinned range of `T` into `dst` at `offset_in_elements`, under bytes_to_buffer's contract.
    template <class T>
    void data_to_buffer(buffer<T> const& dst,
                        std::type_identity_t<cc::pinned_data<T const>> data,
                        isize offset_in_elements = 0)
    {
        data_to_buffer<T>(dst.raw(), cc::move(data), offset_in_elements);
    }

    // Raw overloads — element type supplied by the call site rather than the buffer.
public:
    /// Streams `data` into `buffer` starting at `offset_in_bytes`; an empty pin is a no-op.
    /// The buffer must have been created with buffer_usage::copy_dst, and offset_in_bytes + data.size() must be <= its size.
    /// Build the pin with cc::make_pinned_data / cc::as_pinned_data — that pin is what keeps the upload zero-copy, which is why it is passed rather than a plain span.
    void bytes_to_buffer(raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset_in_bytes = 0);

    /// Streams a trivially-copyable pinned range, re-viewing the SAME pin as bytes (no copy).
    /// `offset_in_elements` is in elements of T, and the rest is bytes_to_buffer's contract.
    template <class T>
    void data_to_buffer(raw_buffer_handle buffer, cc::pinned_data<T const> data, isize offset_in_elements = 0)
    {
        static_assert(std::is_trivially_copyable_v<T>, "upload element type must be trivially copyable");
        bytes_to_buffer(cc::move(buffer), data.as_bytes(),
                        offset_in_elements * isize(sizeof(T))); // as_bytes() shares the owner
    }

    /// Streams tightly-packed pinned `data` into one region of `texture` — the async mirror of cmd.upload.bytes_to_texture.
    /// The pin keeps the source alive until the copy consumes it; a later command list that reads the texture waits on the copy automatically.
    /// Needs texture_usage::copy_dst.
    void bytes_to_texture(raw_texture_handle texture,
                          cc::pinned_data<byte const> data,
                          subresource_index const& subresource = {},
                          cc::optional<texture_region> region = {});

    /// Sets the size of one async-upload staging window in bytes (> 0); the staging buffer is triple-buffered, so this many bytes times three.
    /// Bigger windows amortize submits, smaller ones cut latency and memory.
    /// May be called any time: the copy actor adopts it between windows, draining outstanding copies first, so in-flight uploads are unaffected.
    /// Distinct from set_inline_budget, which sizes the separate inline (cmd.upload) ring.
    void set_async_window_size(isize bytes);

    /// Sets the inline-upload (cmd.upload) ring capacity in bytes (> 0) — the ring inline uploads stage through, bounding the per-epoch inline-upload volume.
    /// May be called any time, repeatedly: it records a *pending* budget applied at the next advance_epoch, which drains in-flight epochs then reallocates.
    /// Distinct from set_async_window_size, which sizes this async (ctx.upload) path's staging buffer.
    void set_inline_budget(isize bytes);

    /// The inline-upload ring a context starts with, unless its backend config says otherwise.
    static constexpr isize default_inline_budget_bytes = isize(16) * 1024 * 1024;

    // Pinned to its owning context: neither copyable nor movable.
    context_upload_scope(context_upload_scope const&) = delete;
    context_upload_scope(context_upload_scope&&) = delete;
    context_upload_scope& operator=(context_upload_scope const&) = delete;
    context_upload_scope& operator=(context_upload_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected backend virtual (mutual friendship).
    friend class context;
    explicit context_upload_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
