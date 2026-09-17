#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/resource/subresource.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/transfer/stream_sink.hh>
#include <shaped-graphics/transfer/stream_source.hh>

/// ctx.stream on WebGPU, and the async tier's transfers along with it.
///
/// **WebGPU has one queue and no copy actor**, so both tiers write straight to the queue with `writeBuffer` / `writeTexture` and read back by mapping.
/// What separates the streaming tier is only pacing: a stream moves at most one window of bytes per pump sweep, so a large one never stalls a frame.
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
    ///
    /// This is the wait a list touching a streamed resource owes, and the one a download does; on WebGPU it costs no wait at all, only the writes brought forward.
    /// A source with nothing ready cannot be waited for, so its stream stays behind and the reader sees what landed so far — that one warns.
    /// A stream nobody promoted warns once when it is flushed for a list, since the caller touched a resource they had claimed.
    void flush_resource(void const* resource, bool for_list);

    /// Whether any write is queued at all, so a caller with nothing to flush pays one check.
    [[nodiscard]] bool has_queued_writes() const { return !_uploads.empty(); }
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

    /// Whether any stream is still moving bytes; a stream counts until its handle settles.
    [[nodiscard]] bool is_idle() const { return _uploads.empty(); }

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
    };

    /// Whether an earlier job into the same resource is still queued.
    [[nodiscard]] bool is_behind_its_family(isize index) const;

    /// Moves chunks of job `index` until its source is done, has nothing ready, or `budget` runs out.
    /// Returns whether the job finished, settling its handle.
    [[nodiscard]] bool advance_job(isize index, isize& budget, bool& progressed);

    /// Records a readback of `buffer` and submits it.
    /// A stream passes its control, whose completion the map settles; a sink receives the bytes instead of a destination, and the future is then empty.
    [[nodiscard]] sg::bytes_future read_buffer(sg::raw_buffer_handle const& buffer,
                                               isize offset,
                                               isize size,
                                               std::shared_ptr<sg::impl::stream_control> control,
                                               sg::stream_sink sink);
    [[nodiscard]] sg::bytes_future read_texture(sg::raw_texture_handle const& texture,
                                                sg::subresource_index const& subresource,
                                                sg::texture_region const& region,
                                                std::shared_ptr<sg::impl::stream_control> control,
                                                sg::stream_sink sink);

    /// One window: the highest-priority uploads move up to a window of bytes between them.
    /// A download needs no window — it is one copy and one map — so it starts at the call and settles from its map.
    [[nodiscard]] bool pump();

    /// Settles `control` once the queue has run everything written so far.
    void settle_after_queue(std::shared_ptr<sg::impl::stream_control> control);

    void ensure_pump();

    webgpu_context* _ctx = nullptr;
    isize _window_bytes = 4 * 1024 * 1024;
    u64 _next_sequence = 1;
    cc::vector<upload_job> _uploads;
    cc::thread_pump_registration _pump;
};
