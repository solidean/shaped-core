#pragma once

#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer_access.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

namespace sg::backend::vulkan
{
/// The VkDevice of a context, for a header that cannot see vulkan_context's definition.
/// Defined in vulkan_buffer.cc, where the context is complete.
[[nodiscard]] VkDevice ctx_device_of(vulkan_context& ctx);
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

    vulkan_context& _ctx;      // creating context — outlives this buffer
    sg::epoch _creation_epoch; // epoch this buffer was created in (immutable identity / diagnostics)
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE; // owned allocation; null for a placed buffer, which owns no memory

    /// The buffer's device address, which is what a descriptor names; 0 for an empty buffer.
    /// Read once at creation rather than per descriptor write, since it cannot change.
    VkDeviceAddress _device_address = 0;

    // The heap a placed buffer sits in, held so the heap outlives the placement; null for a dedicated buffer.
    sg::memory_heap_handle _heap;

    // Guarded because concurrent command lists may record against the same buffer.
    // Mutable so a const handle can still track access: tracking is bookkeeping about the buffer, not a change to it.
    mutable cc::mutex<vulkan_buffer_access> _access;
};
