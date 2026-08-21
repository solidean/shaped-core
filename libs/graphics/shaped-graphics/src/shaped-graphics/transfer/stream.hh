#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh> // typed buffer<T> — the preferred overloads below take it
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/types.hh>

#include <type_traits>

/// Streaming transfer facade for a context, reached as `ctx.stream`.
///
/// The deliberately weaker sibling of `ctx.upload` / `ctx.download`.
/// Those give a **strong scheduling guarantee** — the next command list touching the resource waits on the copy —
/// which is right for must-be-there data and wrong for bulk asset traffic, where the same automatic wait turns
/// "slow" into "stall".
///
/// Streaming rides the same copy queue, windows and actor, and trades that guarantee for a handle: dynamic
/// priority, progress, cancellation, and a `cc::async` completion node other work can depend on.
/// The contract it hands back in exchange, stated once:
///
///   The streamed extent is YOURS ALONE between the call and the moment the handle reports complete.
///   Any command list touching it must be SUBMITTED after you observed that.
///   Everything outside the extent is unaffected.
///
/// "Submitted", not "recorded" — a list recorded during the transfer and submitted after it settles is fine.
/// `stream_scope` is how wide that extent is, checked against the resource's usage flags.
///
/// Dropping a handle cancels its transfer, so a stream always has an observer.
/// If you want fire-and-forget, you want `ctx.upload`: that is the same trade in the other direction.
class sg::context_stream_scope
{
    // Uploads — typed-buffer overload first, the preferred form.
public:
    /// Streams a pinned range of `T` into `dst` at `offset_in_elements`, under bytes_to_buffer's contract.
    template <class T>
    [[nodiscard]] stream_upload_handle data_to_buffer(buffer<T> const& dst,
                                                      std::type_identity_t<cc::pinned_data<T const>> data,
                                                      isize offset_in_elements = 0,
                                                      stream_scope scope = stream_scope::resource)
    {
        static_assert(std::is_trivially_copyable_v<T>, "stream element type must be trivially copyable");
        return bytes_to_buffer(dst.raw(), data.as_bytes(), offset_in_elements * isize(sizeof(T)), scope);
    }

    /// Streams `data` into `buffer` starting at `offset_in_bytes`, returning a handle that controls and observes it.
    /// The buffer needs buffer_usage::copy_dst, and `stream_scope::region` additionally needs
    /// buffer_usage::allow_region_stream.
    /// `stream_scope::subresource` is rejected: buffers have no subresources.
    /// An empty pin yields an already-settled handle and no work.
    [[nodiscard]] stream_upload_handle bytes_to_buffer(raw_buffer_handle buffer,
                                                       cc::pinned_data<byte const> data,
                                                       isize offset_in_bytes = 0,
                                                       stream_scope scope = stream_scope::resource);

    /// Streams tightly-packed pinned `data` into one region of `texture`.
    /// Needs texture_usage::copy_dst, plus allow_subresource_stream / allow_region_stream for the narrower scopes.
    [[nodiscard]] stream_upload_handle bytes_to_texture(raw_texture_handle texture,
                                                        cc::pinned_data<byte const> data,
                                                        subresource_index const& subresource = {},
                                                        cc::optional<texture_region> region = {},
                                                        stream_scope scope = stream_scope::resource);

    // Downloads.
public:
    /// Streams `count` elements of `T` back from `src`, under bytes_from_buffer's contract.
    template <class T>
    [[nodiscard]] stream_download_handle data_from_buffer(buffer<T> const& src,
                                                          isize offset_in_elements,
                                                          isize count,
                                                          stream_scope scope = stream_scope::resource)
    {
        static_assert(std::is_trivially_copyable_v<T>, "stream element type must be trivially copyable");
        auto const stride = isize(sizeof(T));
        return bytes_from_buffer(src.raw(), offset_in_elements * stride, count * stride, scope);
    }

    /// Streams `size_in_bytes` from `buffer` back to the host, returning a handle carrying the destination future.
    /// The buffer needs buffer_usage::copy_src, and the scope rules match bytes_to_buffer's.
    /// A zero-size read yields an already-settled handle over an empty future.
    [[nodiscard]] stream_download_handle bytes_from_buffer(raw_buffer_handle buffer,
                                                           isize offset_in_bytes,
                                                           isize size_in_bytes,
                                                           stream_scope scope = stream_scope::resource);

    /// Streams one region of `texture` back to the host as tightly-packed bytes.
    /// Needs texture_usage::copy_src, plus the scope's usage flag.
    [[nodiscard]] stream_download_handle bytes_from_texture(raw_texture_handle texture,
                                                            subresource_index const& subresource = {},
                                                            cc::optional<texture_region> region = {},
                                                            stream_scope scope = stream_scope::resource);

    // Configuration.
public:
    /// The share of copied bytes streaming is owed over time, in [0, 1]; defaults to 0.1.
    ///
    /// It is a floor on throughput rather than a cap: with no async work pending, streaming gets everything.
    /// Sharing is per WINDOW rather than within one — whichever tier is owed bandwidth fills the current window
    /// first and the other takes what is left.
    /// Splitting each window by ratio reads as more direct and does not survive textures, whose placed footprints
    /// have a minimum viable slice: a reserved fraction too small for one aligned row is simply wasted.
    ///
    /// Upload and download own independent copy queues, so each keeps its own ratio.
    void set_upload_ratio(float ratio);
    void set_download_ratio(float ratio);

    /// Effective streaming priority becomes `priority + factor * seconds_waiting`; defaults to 0, which is off.
    ///
    /// Off by default because a low-priority transfer never running while higher-priority work exists is usually
    /// exactly what the caller meant, not a bug to be corrected.
    void set_upload_aging(float per_second);
    void set_download_aging(float per_second);

    // Pinned to its owning context: neither copyable nor movable.
    context_stream_scope(context_stream_scope const&) = delete;
    context_stream_scope(context_stream_scope&&) = delete;
    context_stream_scope& operator=(context_stream_scope const&) = delete;
    context_stream_scope& operator=(context_stream_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected backend virtual (mutual friendship).
    friend class context;
    explicit context_stream_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
