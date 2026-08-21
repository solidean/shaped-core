#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>

/// Where a streaming upload's bytes come from.
///
/// The default is one pinned blob for the whole destination — the plain "this upload is slow, do not hitch the
/// frame" case — but a stream that must not hold its whole payload resident wants to produce bytes as it goes.
/// That is what this seam is: a lazy sequence of chunks the copy actor pulls from as windows open.

/// Why a poll returned what it did.
/// Modelled as a status rather than an optional because "nothing yet" and "nothing ever" are different answers, and
/// conflating them either stalls a finished transfer or completes an unfinished one.
enum class sg::stream_source_status : sg::u8
{
    /// The poll produced a chunk to transfer.
    ready,

    /// Nothing is available right now, but more is coming.
    /// The system passes this transfer over and fills the window with other work; it does NOT spin on the source.
    /// Once the actor has drained everything else it sleeps, so a source that stalls must eventually call its waker
    /// or the transfer simply waits.
    not_yet,

    /// Every chunk has been produced.
    /// The transfer completes once the chunks already handed over have landed.
    done,

    /// The source cannot continue; the transfer settles on its error channel and stops being served.
    /// This is the only way out for a source that can never produce what it promised — without it, a stalled
    /// transfer would sit in the queue forever, and anything chained onto its completion with it.
    failed,
};

/// One run of bytes and where they belong in the destination.
/// `offset` is measured in bytes from the start of the extent named at the streaming call, not from the resource.
///
/// For a texture the destination is the region's tightly-packed bytes, so a chunk must start and end on a **row**
/// boundary — `offset` and `data.size()` are both multiples of the region's row size.
/// Rows are the smallest unit a texture copy can place, so a part-row chunk has nothing it could be copied into.
struct sg::stream_chunk
{
    cc::pinned_data<byte const> data;
    isize offset = 0;
};

/// The result of one poll: a status, and a chunk that is only meaningful when the status is `ready`.
struct sg::stream_poll
{
    stream_source_status status = stream_source_status::not_yet;
    stream_chunk chunk;
};

/// A lazy sequence of chunks feeding one streaming upload.
///
/// **Polled on the copy actor thread, and it must not block.**
/// That thread is what stages every other transfer in the system, so a source that waits on a file read or a decode
/// stalls all of them; produce elsewhere and only *check* here.
/// `not_yet` is the correct answer to "my data is not back from the loader" — it costs the transfer a window, not
/// the system a thread.
///
/// Chunk order is unconstrained: each carries its own offset, and the streaming contract makes the destination
/// unreadable until the handle settles, so nothing can observe the order they land in.
///
/// The source is destroyed on the actor thread when its transfer ends, cancellation included, so a source holding
/// asyncs gets its cancellation from that destruction rather than from any call here.
class sg::stream_source
{
public:
    virtual ~stream_source() = default;

    /// Produce the next chunk, or say why there is none.
    [[nodiscard]] virtual stream_poll try_next_chunk() = 0;

    /// Best-effort total size in bytes, or < 0 when unknown.
    /// A hint for `progress()` only — compression and lazy production can both make it wrong, so nothing in the
    /// system decides anything on it.
    [[nodiscard]] virtual i64 total_size_hint() const { return -1; }

    /// Installed once when the transfer is admitted; call it when a previous `not_yet` has become answerable.
    ///
    /// Without it a stalled transfer resumes only when some other message wakes the actor, which in a busy system
    /// is constantly and in a quiet one is never.
    /// Safe to call from any thread, any number of times, and after the transfer has ended.
    /// A source that is always ready — the default one — never needs it.
    virtual void set_waker(cc::unique_function<void()> waker) { (void)waker; }
};

namespace sg
{
/// The default source: the whole payload as one already-resident chunk.
/// This is what the plain `bytes_to_buffer(dst, pinned)` form builds, and it is always `ready`.
[[nodiscard]] std::unique_ptr<stream_source> make_pinned_stream_source(cc::pinned_data<byte const> data,
                                                                       isize offset = 0);
} // namespace sg
