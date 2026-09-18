#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/impl/transfer_drain.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/transfer/stream_sink.hh>
#include <shaped-graphics/transfer/stream_source.hh>

/// One streaming transfer as the actor owns it.
///
/// Both directions share the record, because nearly everything about them is the same: a destination, an extent, a
/// cursor, and a completion nobody else may settle.
/// What differs is which side the bytes come from, which `is_download` picks.
struct sg::backend::metal::metal_stream_job
{
    u64 sequence = 0; ///< submission order, and the scheduler's tiebreak
    u64 family = 0;   ///< the destination's address: two transfers of one resource stay in order

    bool is_download = false;
    bool is_texture = false;

    std::shared_ptr<sg::impl::stream_control> control;
    sg::impl::transfer_drain::token drain;

    /// A strong handle for the duration: a caller may drop its last one the moment the call returns, and the copy is
    /// still to come.
    sg::raw_buffer_handle buffer;
    sg::raw_texture_handle texture;

    std::unique_ptr<sg::stream_source> source; ///< uploads only

    isize offset_in_bytes = 0; ///< buffer transfers: where the extent starts in the resource
    isize size_in_bytes = 0;   ///< the extent's total size, tightly packed

    sg::subresource_index subresource;
    sg::texture_region region;
    texture_staging_layout layout; ///< textures only, and the row size chunk offsets are measured in

    sg::stream_sink sink; ///< downloads with a sink; empty for the resident form
    /// Where a resident download's bytes land, as a bare span.
    ///
    /// **A span rather than the `pinned_data`**, because the pin beside it is what says the caller still wants them:
    /// holding a strong pin here keeps the destination alive forever, `weak_destination.lock()` always succeeds, and
    /// dropping the future stops meaning cancelled.
    /// The span is only read while that lock succeeds.
    cc::span<byte> destination;
    std::weak_ptr<void const> weak_destination;
    cc::shared_async<cc::unit> bytes_completion; ///< the resident form's future, settled with the last chunk

    isize cursor = 0;       ///< downloads: the next byte of the extent to read
    isize delivered = 0;    ///< downloads: the next byte to hand to the sink, so delivery stays in order
    isize staged_bytes = 0; ///< progress, in bytes handed to the GPU

    bool source_exhausted = false; ///< the source said `done`, or a download has read its whole extent
    bool failed = false;
    int chunks_in_flight = 0; ///< commits carrying this job's bytes that have not completed

    /// The direct-queue submission this transfer orders behind, read once when it is admitted.
    ///
    /// Per chunk would deadlock, and the loop is short: a list touching a streamed resource waits for the whole
    /// transfer, so a chunk staged after that list was submitted would wait for a list waiting for it.
    /// Reading it at admission is also what the contract says — the extent is the caller's alone from the call
    /// onward, so a list submitted later has no claim to order ahead of the stream.
    u64 direct_wait = 0;

    /// The off-frame transfer this one orders behind, on the transfer system's own timeline, read at admission too.
    ///
    /// The direct queue is not the only writer: `ctx.upload` commits to the transfer queue, and a stream reading a
    /// resource an async upload is still filling would otherwise read whatever was there — zeroes, where the texture
    /// was never written at all.
    pending_transfers transfer_wait;

    /// This transfer's value on its resource's streaming timeline, reserved at admission.
    /// Signalled when the job ends, whichever way it ends — a list waiting on it must never be left waiting.
    u64 stream_value = 0;
};

/// The streaming tier: `ctx.stream`, on the transfer queue the async tier already owns.
///
/// **The scheduling this arbitrates is streaming against streaming, not streaming against async.**
/// dx12 shares one window packer between the two tiers, so its ratio decides who fills the next window.
/// Metal's async transfers never queue at all — unified memory lets the caller stage and commit on the spot — so a
/// stream contends only with other streams, and the ratio knobs have nothing to arbitrate.
/// They are accepted and recorded, because the scheduler is sg's and its shape is not this backend's to change.
///
/// One actor owns every job and every cycle.
/// A cycle polls each eligible job once, stages what it gets into one buffer for the whole batch, and commits it —
/// so a source that answers `not_yet` costs its own transfer a cycle rather than costing the system its thread.
///
/// Delivery runs on the actor too, never in the commit handler: a sink must be called in cursor order and on one
/// thread, and Metal's handlers arrive on a dispatch queue that promises neither.
/// The handler's whole job is to tell the actor a batch has landed.
class sg::backend::metal::metal_stream_system
{
public:
    void create(metal_context& ctx);

    /// Cancels everything still in flight and joins the actor.
    /// Every job settles its completion on the way out — a manual node nobody pushes parks its dependents forever.
    void shutdown();

    [[nodiscard]] sg::stream_upload_handle upload_to_buffer(sg::raw_buffer_handle buffer,
                                                            std::unique_ptr<sg::stream_source> source,
                                                            isize offset_in_bytes);

    [[nodiscard]] sg::stream_upload_handle upload_to_texture(sg::raw_texture_handle texture,
                                                             std::unique_ptr<sg::stream_source> source,
                                                             sg::subresource_index const& subresource,
                                                             sg::texture_region const& region);

    /// `sink` empty means the resident form, whose bytes the returned handle's future carries.
    [[nodiscard]] sg::stream_download_handle download_from_buffer(sg::raw_buffer_handle buffer,
                                                                  sg::stream_sink sink,
                                                                  isize offset_in_bytes,
                                                                  isize size_in_bytes);

    [[nodiscard]] sg::stream_download_handle download_from_texture(sg::raw_texture_handle texture,
                                                                   sg::stream_sink sink,
                                                                   sg::subresource_index const& subresource,
                                                                   sg::texture_region const& region);

    void set_upload_ratio(float ratio);
    void set_download_ratio(float ratio);
    void set_upload_aging(float per_second);
    void set_download_aging(float per_second);

    /// Whether any transfer is still in flight — what `block_until_transfers_drained` waits on.
    [[nodiscard]] bool has_pending() const { return !_drain.is_idle(); }

    /// Wakes the actor if any stream is in flight, so a job held back on a direct-queue submission is re-examined.
    ///
    /// Called from a shared-event notification, on Apple's thread, through the detachable sink — which is why it is
    /// gated on `has_pending()` rather than on a flag the actor keeps: the drain token exists from before a job is
    /// admitted until after it settles, so there is no window where a notification finds nothing to wake.
    void wake_if_pending();

    /// The listener a gated job's wake is armed on.
    /// Its own, rather than the completion waiter's: that one's armed values live without a lock precisely because it
    /// is the only caller of its listener.
    [[nodiscard]] MTL::SharedEventListener* listener() const { return _listener; }

    /// Releases that listener.
    /// Separate from `shutdown` because it must happen after the epoch system has drained the queue, at which point
    /// every armed value has been reached and no notification is still due.
    void release_listener();

private:
    class actor_impl;

    /// Admits a fully-built job, settling it immediately where there is nothing to do.
    void admit(metal_stream_job job);

    /// Builds the destination a resident download lands in, then admits the job.
    [[nodiscard]] sg::stream_download_handle finish_download(metal_stream_job job);

    metal_context* _ctx = nullptr;
    cc::unique_ptr<cc::threaded_actor<metal_stream_job, u64>> _actor;
    actor_impl* _impl = nullptr; // owned by _actor; reachable for the ratio knobs
    sg::impl::transfer_drain _drain;
    cc::atomic<u64> _next_sequence = {1};
    MTL::SharedEventListener* _listener = nullptr;
};
