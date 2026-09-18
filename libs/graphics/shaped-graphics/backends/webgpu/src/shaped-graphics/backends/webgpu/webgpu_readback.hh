#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/bytes_future.hh>

/// One readback: a staging buffer a GPU copy writes, and what to do with its bytes once it maps.
///
/// Built when the copy is recorded, and mapped only after the submit carrying the copy — mapping waits for the queue, so that is what orders the read after the write.
struct sg::backend::webgpu::webgpu_readback
{
    /// A MAP_READ | COPY_DST buffer from the pool, returned to it after the map.
    wgpu_buffer staging;
    isize staging_capacity = 0;

    /// How many staged bytes to map.
    isize mapped_bytes = 0;

    /// Receives the mapped bytes; strips any row padding into the destination.
    /// Returns false to fail the transfer, which a sink does.
    cc::unique_function<bool(cc::span<byte const>)> deliver;

    /// The destination's owner, held weakly: expired means the caller dropped the future, which is a cancellation.
    /// Null for a readback whose delivery owns what it writes into, like a sink's.
    std::weak_ptr<void const> pin;
    bool has_pin = false;

    cc::shared_async<cc::unit> completion;
    std::shared_ptr<sg::bytes_wait_gate> gate;
};

/// The readback side of every download: the pool of MAP_READ staging buffers, and the maps in flight.
///
/// WebGPU cannot map a buffer the GPU writes, only a MAP_READ copy of it, and maps asynchronously.
/// So a download is a copy into a staging buffer, then a map after the submit, then a copy out in the map's callback — no thread, no ring and no wait.
class sg::backend::webgpu::webgpu_readback_pool
{
public:
    void initialize(webgpu_context& ctx);

    /// Drops the pooled buffers; maps still in flight settle through the callback anchor, cancelled.
    void shutdown();

    /// A staging buffer holding at least `size_in_bytes`, whole words.
    [[nodiscard]] webgpu_readback acquire(isize size_in_bytes);

    /// Starts the map of a readback whose copy has been submitted.
    void start_map(webgpu_readback job);

    /// Cancels a readback whose copy never ran, returning its buffer.
    void discard(webgpu_readback& job);

    /// Whether every mapping readback has been delivered.
    [[nodiscard]] bool is_idle() const { return _outstanding == 0; }

    /// Called from the map callback with the context still alive.
    void finish_map(webgpu_readback& job, WGPUMapAsyncStatus status);

private:
    void release(wgpu_buffer buffer, isize capacity);

    webgpu_context* _ctx = nullptr;

    struct pooled
    {
        wgpu_buffer buffer;
        isize capacity = 0;
    };
    cc::vector<pooled> _free;

    // Maps started and not yet finished.
    // A plain count rather than an sg::impl::transfer_drain: a map can finish after the context is gone, and a drain token would then reach back into it.
    isize _outstanding = 0;
};
