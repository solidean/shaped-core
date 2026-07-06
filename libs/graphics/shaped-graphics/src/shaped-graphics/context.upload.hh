#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/fwd.hh>

#include <type_traits>

namespace sg
{
/// Async host→device upload facade for a context, reached as `ctx.upload`.
///
/// The context-level mirror of the inline `cmd.upload`: `cmd.upload` records a copy inline in a command
/// list (visible to later commands in that list), while `ctx.upload` streams the copy on a dedicated
/// copy queue, off the frame path. It is the right tool for bulk asset streaming, not small must-be-
/// visible-now per-frame writes.
///
/// Fire-and-forget: the call returns immediately and the copy runs asynchronously. The source bytes are
/// held alive by the pin until the copy has consumed them, so the caller may free the original right
/// away (unlike inline upload, which copies synchronously). A later command list that reads the buffer
/// automatically waits on the copy — no manual synchronization. Uploads to one buffer keep their
/// submission order; across buffers the order is unconstrained.
///
/// A thin facade over its owning context: it forwards each op to the context's backend impl.
class context_upload_scope
{
public:
    /// Streams `data` into `buffer` starting at `offset_in_bytes`. The buffer must have been created with
    /// buffer_usage::copy_dst. An empty pin is a no-op. Precondition: offset_in_bytes + data.size() <=
    /// buffer size. Build the pin with cc::make_pinned_data / cc::as_pinned_data; that pin is what keeps
    /// the upload zero-copy, which is why it is passed rather than a plain span.
    void bytes_to_buffer(buffer_handle buffer, cc::pinned_data<cc::byte const> data, cc::isize offset_in_bytes = 0);

    /// Streams a trivially-copyable pinned range, re-viewing the SAME pin as bytes (no copy). See
    /// bytes_to_buffer.
    template <class T>
    void data_to_buffer(buffer_handle buffer, cc::pinned_data<T const> data, cc::isize offset_in_bytes = 0)
    {
        static_assert(std::is_trivially_copyable_v<T>, "upload element type must be trivially copyable");
        bytes_to_buffer(cc::move(buffer), data.as_bytes(), offset_in_bytes); // as_bytes() shares the owner
    }

    // Pinned to its owning context: neither copyable nor movable.
    context_upload_scope(context_upload_scope const&) = delete;
    context_upload_scope(context_upload_scope&&) = delete;
    context_upload_scope& operator=(context_upload_scope const&) = delete;
    context_upload_scope& operator=(context_upload_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected backend
    // virtual (mutual friendship).
    friend class context;
    explicit context_upload_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
