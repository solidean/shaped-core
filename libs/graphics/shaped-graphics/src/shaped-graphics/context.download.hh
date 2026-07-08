#pragma once

#include <clean-core/common/utility.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

#include <type_traits>

namespace sg
{
/// Async device→host download facade for a context, reached as `ctx.download`.
///
/// The context-level mirror of the inline `cmd.download`: `cmd.download` records the readback inline in a
/// command list (its future is delivered once that list runs), while `ctx.download` streams the readback
/// on the dedicated copy queue, off the frame path. It is the right tool for bulk readback — asset baking,
/// screenshots, GPU→CPU result streaming — not small must-be-back-now per-frame reads.
///
/// Returns a bytes_future immediately; the copy runs asynchronously on the copy queue and the future
/// becomes ready once the bytes have landed in the host destination (block on it with ctx.wait_for). No
/// manual synchronization: the read automatically waits on the last command list that wrote the buffer,
/// and a later command list that writes the buffer automatically waits on the read. Dropping the future
/// cancels the copy at the next opportunity.
///
/// This scope also exposes context-wide settings for the readback rings downloads stage through. A thin
/// facade over its owning context: it forwards each op to the context's backend impl.
class context_download_scope
{
public:
    /// Streams `size_in_bytes` from `buffer` starting at `offset_in_bytes` back to the host on the copy
    /// queue. The buffer must have been created with buffer_usage::copy_src. Returns a bytes_future that
    /// becomes ready once the copy has landed the bytes in the host destination. A zero-size read yields an
    /// already-ready, empty future. Precondition: offset_in_bytes + size_in_bytes <= buffer size.
    [[nodiscard]] bytes_future bytes_from_buffer(raw_buffer_handle buffer,
                                                 cc::isize offset_in_bytes,
                                                 cc::isize size_in_bytes);

    /// Downloads `count` elements of a trivially-copyable type; `offset_in_elements` and `count` are in
    /// elements of T. See bytes_from_buffer.
    template <class T>
    [[nodiscard]] data_future<T> data_from_buffer(raw_buffer_handle buffer, cc::isize offset_in_elements, cc::isize count)
    {
        static_assert(std::is_trivially_copyable_v<T>, "download element type must be trivially copyable");
        auto const stride = cc::isize(sizeof(T));
        return data_future<T>(bytes_from_buffer(cc::move(buffer), offset_in_elements * stride, count * stride));
    }

    /// Sets the size of one async-download staging window in bytes (> 0); the staging buffer is triple-
    /// buffered, so this many bytes times three. Bigger windows amortize submits, smaller ones cut latency
    /// and memory. May be called any time: the copy actor adopts it between windows (draining outstanding
    /// copies first), so in-flight downloads are unaffected. Distinct from set_budget, which sizes the
    /// separate inline (cmd.download) readback ring.
    void set_async_window_size(cc::isize bytes);

    /// Sets the inline readback ring capacity in bytes (> 0) — the ring all `cmd.download` readbacks stage
    /// through, bounding the in-flight inline-download volume. May be called any time, repeatedly: it
    /// records a *pending* budget and returns immediately without touching the GPU. The change takes effect
    /// at the next advance_epoch, which drains outstanding readbacks and reallocates the ring; until then
    /// the current budget stays in force. Backends without an inline-download path ignore it.
    void set_budget(cc::isize bytes);

    // Pinned to its owning context: neither copyable nor movable.
    context_download_scope(context_download_scope const&) = delete;
    context_download_scope(context_download_scope&&) = delete;
    context_download_scope& operator=(context_download_scope const&) = delete;
    context_download_scope& operator=(context_download_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected backend
    // virtual (mutual friendship).
    friend class context;
    explicit context_download_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
