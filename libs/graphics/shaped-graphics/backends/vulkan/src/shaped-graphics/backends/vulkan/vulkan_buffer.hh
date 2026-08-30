#pragma once

#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer_access.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

namespace sg::backend::vulkan
{
/// The VkDevice of a context, for a header that cannot see vulkan_context's definition.
/// Defined in vulkan_buffer.cc, where the context is complete.
[[nodiscard]] VkDevice ctx_device_of(vulkan_context& ctx);

/// A completion group from the context's pool, for the same reason.
[[nodiscard]] vulkan_completion_group_handle ctx_acquire_completion_group(vulkan_context& ctx);
} // namespace sg::backend::vulkan

/// Vulkan implementation of sg::raw_buffer.
/// Holds the VkBuffer and its backing device-local VkDeviceMemory — sg exposes no host-visible buffers.
/// Both are VK_NULL_HANDLE for an empty (size 0) buffer.
class sg::backend::vulkan::vulkan_buffer final : public sg::raw_buffer
{
public:
    vulkan_buffer(vulkan_context& ctx,
                  sg::epoch created_in,
                  isize size_in_bytes,
                  sg::buffer_usages usage,
                  VkBuffer buffer,
                  VkDeviceMemory memory,
                  sg::memory_heap_handle heap = nullptr)
      : sg::raw_buffer(size_in_bytes, usage),
        _ctx(ctx),
        _creation_epoch(created_in),
        _buffer(buffer),
        _memory(memory),
        _heap(cc::move(heap))
    {
        // A timeline only where a transfer is even possible, so a buffer nothing can copy costs no semaphore.
        if (usage.has(sg::buffer_usage::copy_dst))
            _upload_group = ctx_acquire_completion_group(ctx);
        if (usage.has(sg::buffer_usage::copy_src))
            _download_group = ctx_acquire_completion_group(ctx);

        if (_buffer != VK_NULL_HANDLE)
        {
            auto const info = VkBufferDeviceAddressInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = _buffer,
            };
            _device_address = vkGetBufferDeviceAddress(ctx_device_of(ctx), &info);
        }
    }

    // Deferred deletion: hands the GPU handles and finalizers to the context, freed once the owning epoch retires.
    // Freeing them here could pull memory out from under a GPU still reading them.
    // Body in vulkan_buffer.cc.
    ~vulkan_buffer() override;

protected:
    // A transient buffer expires when its epoch advances, and its storage goes then rather than at destruction:
    // the heap reuses those bytes for the next epoch either way, so holding a handle must not hold the memory.
    void on_expired() const override;

public:
    /// Accumulate one declared access for `slot`'s next op, seeding a fresh slot from the canonical state.
    /// Call once per binding; several declares for one op merge into a single barrier at flush.
    /// Thread-safe.
    void declare_access(sg::command_list_slot slot, sg::pipeline_stage_flags stages, sg::access_flags access) const
    {
        _access.lock([&](vulkan_buffer_access& a) { a.declare(slot, stages, access); });
    }

    /// Test-and-set the per-op pending flag; true only the first time since the last flush.
    /// The command list uses it to enqueue this buffer for the pre-op flush exactly once, however often it is bound.
    /// Thread-safe.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_buffer_access& a) { return a.mark_pending_barrier(slot); });
    }

    /// Test-and-set the finalize-set flag; true only the first time for this slot, until the slot is cleared.
    /// Thread-safe.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_buffer_access& a) { return a.mark_recorded(slot); });
    }

    /// The single barrier satisfying everything declared for `slot` since the last flush.
    /// Called once per op, immediately before it, after all its bindings are declared.
    /// Thread-safe.
    [[nodiscard]] sg::access_barrier flush_access(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_buffer_access& a) { return a.flush(slot); });
    }

    /// `slot`'s list was submitted: what it leaves in flight becomes what the next list synchronizes against.
    /// Unlike dx12, this is not a no-op — see vulkan_buffer_access.hh for why a buffer needs between-lists state.
    /// Thread-safe.
    void finalize_slot(sg::command_list_slot slot) const
    {
        _access.lock([&](vulkan_buffer_access& a) { a.finalize(slot); });
    }

    /// `slot`'s list was dropped: its work never runs, so it leaves nothing behind.
    /// Thread-safe.
    void discard_slot(sg::command_list_slot slot) const
    {
        _access.lock([&](vulkan_buffer_access& a) { a.discard(slot); });
    }

    /// Stages the GPU handles for deferred deletion and clears them; idempotent.
    /// Both expiry and destruction run it, and whichever comes first owns the release.
    void release_storage() const;

    vulkan_context& _ctx;      // creating context — outlives this buffer
    sg::epoch _creation_epoch; // epoch this buffer was created in (immutable identity / diagnostics)
    // Mutable because release_storage() is const: expiry is a lifetime event on a const handle, the same reason
    // sg::raw_buffer::expire() is const.
    mutable VkBuffer _buffer = VK_NULL_HANDLE;
    mutable VkDeviceMemory _memory = VK_NULL_HANDLE; // owned allocation; null for a placed buffer, which owns no memory

    /// The buffer's device address, which is what a descriptor names; 0 for an empty buffer.
    /// Read once at creation rather than per descriptor write, since it cannot change.
    VkDeviceAddress _device_address = 0;

    // The heap a placed buffer sits in, held so the heap outlives the placement; null for a dedicated buffer.
    sg::memory_heap_handle _heap;

    // The completion timelines this resource's transfers run on — one per direction, fixed for its lifetime.
    // Immutable after construction, which is what lets the record path read them with no lock.
    vulkan_completion_group_handle _upload_group;
    vulkan_completion_group_handle _download_group;

    // Four per-resource cross-queue stamps that make the CPU timeline (submit -> async transfer -> submit) mirror GPU
    // ordering between the graphics queue and the transfer queues.
    // All only ever grow and are never reset — a stale value just yields a cheap already-satisfied wait.
    // Mutable and atomic, so they can be stamped through a const handle from any thread.
    // Distinct from the access-state tracking below: that orders graphics-queue lists against each other, while these
    // order a transfer queue against the graphics queue.

    // Forward: highest value an ASYNC upload here will signal on _upload_group.
    // A later graphics-queue list that touches this buffer waits for it at submit, so it sees the async write.
    mutable cc::atomic<u64> _pending_async_upload_value = {0};

    // Reverse: highest graphics-queue submission token of a command list that used this buffer.
    // An async upload defers behind it, so it never overwrites the buffer while an earlier list still reads it; the
    // async download reuses it, so a readback only runs after the last writer finished.
    mutable cc::atomic<u64> _last_used_submission_token = {0};

    // Forward for the async DOWNLOAD: highest value a pending readback here will signal on _download_group.
    // A later graphics-queue list that WRITES this buffer waits for it, so it never overwrites bytes still being read.
    mutable cc::atomic<u64> _pending_async_download_value = {0};

    // Lifetime-only twins of the two async stamps, for STREAMING transfers.
    //
    // Deliberately separate: an async stamp doubles as the forward wait, and streaming must not buy the
    // deferred-deletion gate at the price of making every later list wait on it.
    // Deferred deletion gates on the max within each direction; command-list access tracking reads only the async
    // ones, which is what makes a streamed transfer invisible to a later list until promote_to_async.
    //
    // One per direction rather than one shared: a value is meaningless on a timeline that did not issue it, and the
    // two directions are two different groups.
    mutable cc::atomic<u64> _pending_stream_copy_value = {0};
    mutable cc::atomic<u64> _pending_stream_download_value = {0};

    // Guarded because concurrent command lists may record against the same buffer.
    // Mutable so a const handle can still track access: tracking is bookkeeping about the buffer, not a change to it.
    mutable cc::mutex<vulkan_buffer_access> _access;
};
