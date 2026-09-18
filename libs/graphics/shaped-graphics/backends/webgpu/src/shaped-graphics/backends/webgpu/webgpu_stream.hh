#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/backends/webgpu/webgpu_readback.hh>
#include <shaped-graphics/resource/subresource.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/transfer/stream_sink.hh>
#include <shaped-graphics/transfer/stream_source.hh>

/// ctx.stream on WebGPU, and the async tier's transfers along with it.
///
/// **WebGPU has one queue and no copy actor**, so both tiers write straight to the queue with `writeBuffer` / `writeTexture` and read back by mapping.
/// What separates the streaming tier is pacing: a pump sweep moves at most one window of bytes across every stream, in both directions, so a large one never stalls a frame.
/// An upload chunk larger than what is left of the window is split, on whole words or whole rows; a download is read back one window at a time, the next window only once the last has been delivered.
/// Its handle can be cancelled or reprioritized between windows.
/// A transfer lands in queue order, so a list submitted after one sees its bytes: the automatic synchronization the async tier promises is free here.
///
/// The pump runs wherever the host loop sweeps pumps, which in a browser is the main thread between tasks.
class sg::backend::webgpu::webgpu_stream_system
{
public:
    void initialize(webgpu_context& ctx, isize window_bytes);

    /// Cancels every stream still in flight, settling its handle.
    void shutdown();

    // The async tier: immediate queue writes, and a readback settled from its map.

    /// Writes at once, unless a stream into the same resource is still in flight: then it queues behind that stream, so the later write still wins.
    void upload_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset);
    void upload_texture(sg::raw_texture_handle texture,
                        cc::pinned_data<byte const> data,
                        sg::subresource_index const& subresource,
                        sg::texture_region const& region);

    /// Moves every queued write into `resource` onto the queue now, in order, for work about to read it.
    /// For a list it also copies every remaining window of a stream download reading `resource`, so the list cannot overtake the read.
    ///
    /// This is the wait a list touching a streamed resource owes, and the one a download does; on WebGPU it costs no wait at all, only the writes brought forward.
    /// A source with nothing ready cannot be waited for, so its stream stays behind and the reader sees what landed so far — that one warns.
    /// A stream nobody promoted warns once when it is flushed for a list, since the caller touched a resource they had claimed.
    void flush_resource(void const* resource, bool for_list);

    /// Whether any stream could need flushing for a list — a queued write, or a download still reading — so a caller with nothing to flush pays one check.
    [[nodiscard]] bool has_pending_streams() const { return !_uploads.empty() || !_downloads.empty(); }
    [[nodiscard]] sg::bytes_future download_buffer(sg::raw_buffer_handle buffer, isize offset, isize size);
    [[nodiscard]] sg::bytes_future download_texture(sg::raw_texture_handle texture,
                                                    sg::subresource_index const& subresource,
                                                    sg::texture_region const& region);

    // The streaming tier.

    [[nodiscard]] sg::stream_upload_handle stream_to_buffer(sg::raw_buffer_handle buffer,
                                                            std::unique_ptr<sg::stream_source> source,
                                                            isize offset);
    [[nodiscard]] sg::stream_upload_handle stream_to_texture(sg::raw_texture_handle texture,
                                                             std::unique_ptr<sg::stream_source> source,
                                                             sg::subresource_index const& subresource,
                                                             sg::texture_region const& region);
    [[nodiscard]] sg::stream_download_handle stream_from_buffer(sg::raw_buffer_handle buffer,
                                                                sg::stream_sink sink,
                                                                isize offset,
                                                                isize size);
    [[nodiscard]] sg::stream_download_handle stream_from_texture(sg::raw_texture_handle texture,
                                                                 sg::stream_sink sink,
                                                                 sg::subresource_index const& subresource,
                                                                 sg::texture_region const& region);

    void set_window_bytes(isize bytes) { _window_bytes = bytes > 0 ? bytes : _window_bytes; }

    /// Whether any stream still has bytes to move; an upload leaves once its source is done, before its handle settles from the queue.
    [[nodiscard]] bool is_idle() const { return _uploads.empty() && _downloads.empty(); }

private:
    struct upload_job
    {
        std::shared_ptr<sg::impl::stream_control> control;
        std::unique_ptr<sg::stream_source> source;
        sg::raw_buffer_handle buffer;
        sg::raw_texture_handle texture;
        isize offset = 0;
        sg::subresource_index subresource;
        sg::texture_region region;
        u64 sequence = 0;

        /// The destination resource: jobs sharing one run strictly in sequence order.
        void const* family = nullptr;

        /// An async-tier write queued behind a stream; it has no handle, and nothing to warn about.
        bool is_async = false;

        /// The chunk being moved, and how many of its bytes already went, when a window ended inside it.
        cc::optional<sg::stream_chunk> pending;
        isize pending_done = 0;
    };

    /// A streaming download, read back one window at a time.
    struct download_job
    {
        std::shared_ptr<sg::impl::stream_control> control;
        sg::raw_buffer_handle buffer;
        sg::raw_texture_handle texture;
        sg::subresource_index subresource;
        sg::texture_region region;

        /// A buffer's extent in bytes; a texture's in block rows.
        isize offset = 0;
        isize total = 0;
        isize issued = 0;

        /// Where the bytes go: the sink, shared with the window delivering into it, or else the destination the handle's future owns, held weakly.
        std::shared_ptr<sg::stream_sink> sink;
        cc::span<byte> destination;
        std::weak_ptr<void const> pin;

        /// Windows whose copy is on the queue and whose map has not started, oldest first.
        /// A list touching the resource copies every remaining window at once, so the read lands ahead of it; delivery stays one window per sweep.
        cc::vector<webgpu_readback> copied;

        /// The window being mapped, null between windows.
        cc::shared_async<cc::unit> window;

        u64 sequence = 0;
    };

    /// Records and submits the copy of the next window of `job`, at most `budget` bytes, and returns its readback, not yet mapped.
    [[nodiscard]] webgpu_readback copy_download_window(download_job& job, isize budget);

    /// Moves every queued write into `resource` onto the queue now, in order; flush_resource's upload half.
    void flush_uploads_into(void const* resource, bool for_list);

    /// Whether an earlier job into the same resource is still queued.
    [[nodiscard]] bool is_behind_its_family(isize index) const;

    /// Moves chunks of job `index` until its source is done, has nothing ready, or `budget` runs out.
    /// Returns whether the job finished, settling its handle.
    [[nodiscard]] bool advance_job(isize index, isize& budget, bool& progressed);

    /// The async tier's readback: one copy and one map, started at the call.
    [[nodiscard]] sg::bytes_future read_buffer(sg::raw_buffer_handle const& buffer, isize offset, isize size);
    [[nodiscard]] sg::bytes_future read_texture(sg::raw_texture_handle const& texture,
                                                sg::subresource_index const& subresource,
                                                sg::texture_region const& region);

    /// One window: the highest-priority uploads and the downloads move up to a window of bytes between them.
    [[nodiscard]] bool pump();

    /// Settles `control` once the queue has run everything written so far.
    void settle_after_queue(std::shared_ptr<sg::impl::stream_control> control);

    void ensure_pump();

    webgpu_context* _ctx = nullptr;
    isize _window_bytes = 4 * 1024 * 1024;
    u64 _next_sequence = 1;
    cc::vector<upload_job> _uploads;
    cc::vector<download_job> _downloads;
    cc::thread_pump_registration _pump;
};
