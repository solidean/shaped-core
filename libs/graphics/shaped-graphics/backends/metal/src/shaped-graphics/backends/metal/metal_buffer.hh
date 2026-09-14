#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_resource_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

#include <atomic>

/// Metal implementation of sg::raw_buffer.
///
/// One MTLBuffer and nothing beside it — Metal has no separate allocation object the way Vulkan does, so a placed
/// buffer differs from a dedicated one only in which call minted it.
/// The pointer is null for an empty (size 0) buffer, which is the representation rather than a failure: Metal refuses a
/// zero-length buffer outright, and the validation layer aborts on the attempt.
class sg::backend::metal::metal_buffer final : public sg::raw_buffer
{
public:
    metal_buffer(metal_context& ctx,
                 isize size_in_bytes,
                 sg::buffer_usages usage,
                 MTL::Buffer* buffer,
                 sg::memory_heap_handle heap = nullptr)
      : sg::raw_buffer(size_in_bytes, usage), _ctx(ctx), _buffer(buffer), _heap(cc::move(heap))
    {
    }

    ~metal_buffer() override;

    [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }

    /// The buffer's GPU address, or 0 for an empty one.
    /// This is what an MTL4 argument table binds — Metal 4 binds an address rather than an object.
    [[nodiscard]] u64 gpu_address() const { return _buffer != nullptr ? u64(_buffer->gpuAddress()) : 0; }

    /// Access tracking, shared by every command list recording against this buffer.
    /// Mutable because a command list declares against a `raw_buffer_handle`, which is a handle to const.
    [[nodiscard]] cc::mutex<metal_resource_access>& access() const { return _access; }

    /// The direct-queue submission that last named this buffer, or 0 when none has.
    ///
    /// The two queues are independent timelines, so each direction of the hazard needs its own stamp: a command list
    /// waits on `metal_transfer_system::pending_value_for` before it runs, and an off-frame transfer waits on this.
    /// Without it an async download reads whatever was in the buffer before the list that filled it ran.
    [[nodiscard]] u64 last_used_submission() const { return _last_used_submission.load(std::memory_order_acquire); }

    /// Raises the stamp to `value`, never lowering it.
    /// Lists on different threads may stamp out of order, and the wait has to cover the newest of them.
    void stamp_submission(u64 value) const;

private:
    // Deferred deletion: hands the MTLBuffer to the context, released once the owning epoch retires.
    // Releasing it here could pull memory out from under a GPU still reading it.
    void on_expired() const override;

    /// Releases the MTLBuffer exactly once, whoever gets there first.
    void release_storage() const;

    metal_context& _ctx;
    mutable MTL::Buffer* _buffer = nullptr; // mutable: release_storage runs from the const lifetime hooks
    sg::memory_heap_handle _heap;           // keeps a placed buffer's heap alive; null when dedicated
    mutable cc::mutex<metal_resource_access> _access;
    mutable std::atomic<u64> _last_used_submission = {0}; // mutable: a list declares against a handle to const
};
