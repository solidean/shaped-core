#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_descriptor_heap.hh>

namespace sg::backend::vulkan
{
namespace
{
[[nodiscard]] isize align_up(isize value, isize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

cc::result<cc::unit> vulkan_descriptor_heap::initialize(vulkan_context& ctx,
                                                        isize capacity_in_bytes,
                                                        float transient_fraction)
{
    CC_ASSERT(capacity_in_bytes > 0, "the descriptor heap needs a non-zero capacity");
    CC_ASSERT(transient_fraction >= 0.0f && transient_fraction < 1.0f, "transient fraction must be in [0, 1)");

    _ctx = &ctx;
    _capacity = capacity_in_bytes;
    _alignment = isize(ctx.descriptor_buffer_properties().descriptorBufferOffsetAlignment);
    CC_ASSERT(_alignment > 0, "the device reports a zero descriptor offset alignment");

    // The persistent region is everything below the split; the transient one everything above.
    _transient_begin = align_up(isize(float(capacity_in_bytes) * (1.0f - transient_fraction)), _alignment);

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(capacity_in_bytes),
        // Both descriptor-buffer usages, because one heap serves resource and sampler descriptors alike — Vulkan puts
        // them in the same set layout, unlike D3D12's separate heaps.
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
               | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (VkResult const r = vkCreateBuffer(ctx._device, &info, nullptr, &_buffer); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (descriptor heap) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx._device, _buffer, &requirements);

    // Host-visible because the CPU writes descriptors into it; coherent so no flush is needed before a shader reads.
    u32 const type = ctx.find_memory_type(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(ctx._device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return cc::error("no host-visible coherent memory type for the descriptor heap");
    }

    auto const flags = VkMemoryAllocateFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (VkResult const r = vkAllocateMemory(ctx._device, &alloc, nullptr, &_memory); r != VK_SUCCESS)
    {
        vkDestroyBuffer(ctx._device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return vulkan_error(r, "vkAllocateMemory (descriptor heap) failed");
    }

    if (VkResult const r = vkBindBufferMemory(ctx._device, _buffer, _memory, 0); r != VK_SUCCESS)
    {
        shutdown();
        return vulkan_error(r, "vkBindBufferMemory (descriptor heap) failed");
    }

    void* mapped = nullptr;
    if (VkResult const r = vkMapMemory(ctx._device, _memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS)
    {
        shutdown();
        return vulkan_error(r, "vkMapMemory (descriptor heap) failed");
    }
    _mapped = static_cast<byte*>(mapped);

    auto const address_info = VkBufferDeviceAddressInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _buffer,
    };
    _device_address = vkGetBufferDeviceAddress(ctx._device, &address_info);

    _state.lock([&](heap_state& s) { s.free_ranges.push_back(free_range{.offset = 0, .size = _transient_begin}); });
    return cc::unit{};
}

void vulkan_descriptor_heap::shutdown()
{
    if (_ctx == nullptr)
        return;

    if (_memory != VK_NULL_HANDLE)
    {
        if (_mapped != nullptr)
            vkUnmapMemory(_ctx->_device, _memory);
        vkFreeMemory(_ctx->_device, _memory, nullptr);
    }
    if (_buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(_ctx->_device, _buffer, nullptr);

    _mapped = nullptr;
    _memory = VK_NULL_HANDLE;
    _buffer = VK_NULL_HANDLE;
    _device_address = 0;
    _capacity = 0;
    _ctx = nullptr;
}

vulkan_descriptor_range vulkan_descriptor_heap::allocate_persistent(isize size_in_bytes)
{
    CC_ASSERT(_mapped != nullptr, "the descriptor heap is not initialized");
    if (size_in_bytes <= 0)
        return {};

    isize const needed = align_up(size_in_bytes, _alignment);
    return _state.lock(
        [&](heap_state& s) -> vulkan_descriptor_range
        {
            // First fit.
            // A descriptor range is small and long-lived, so the scan is short and a size-bucketed allocator would buy
            // nothing measurable here.
            for (isize i = 0; i < isize(s.free_ranges.size()); ++i)
            {
                auto& f = s.free_ranges[i];
                if (f.size < needed)
                    continue;

                auto const offset = f.offset;
                f.offset += needed;
                f.size -= needed;
                if (f.size == 0)
                    s.free_ranges.remove_at(i);
                return vulkan_descriptor_range{.offset = offset, .size = needed, .transient = false};
            }
            // Exhausted, reported as an empty range rather than an assert.
            // A large bindless table is exactly the case that hits this, and the caller turns it into a recoverable
            // error.
            return {};
        });
}

void vulkan_descriptor_heap::free_persistent(vulkan_descriptor_range range)
{
    if (range.is_empty() || range.transient)
        return;

    _state.lock(
        [&](heap_state& s)
        {
            // Insert sorted, then coalesce with the neighbour on each side, so a freed range does not fragment the
            // heap merely by the order it was released in.
            isize at = 0;
            while (at < isize(s.free_ranges.size()) && s.free_ranges[at].offset < range.offset)
                ++at;
            s.free_ranges.insert_at(at, free_range{.offset = range.offset, .size = range.size});

            if (at + 1 < isize(s.free_ranges.size())
                && s.free_ranges[at].offset + s.free_ranges[at].size == s.free_ranges[at + 1].offset)
            {
                s.free_ranges[at].size += s.free_ranges[at + 1].size;
                s.free_ranges.remove_at(at + 1);
            }
            if (at > 0 && s.free_ranges[at - 1].offset + s.free_ranges[at - 1].size == s.free_ranges[at].offset)
            {
                s.free_ranges[at - 1].size += s.free_ranges[at].size;
                s.free_ranges.remove_at(at);
            }
        });
}

vulkan_descriptor_range vulkan_descriptor_heap::allocate_transient(isize size_in_bytes)
{
    CC_ASSERT(_mapped != nullptr, "the descriptor heap is not initialized");
    if (size_in_bytes <= 0)
        return {};

    isize const needed = align_up(size_in_bytes, _alignment);
    isize const region = _capacity - _transient_begin;
    CC_ASSERT(needed <= region, "a transient binding group larger than the whole transient region cannot be allocated");

    return _state.lock(
        [&](heap_state& s) -> vulkan_descriptor_range
        {
            // A table must be contiguous, so a request that would straddle the seam restarts at the region's top and
            // the tail is wasted — the same rule the transfer rings follow.
            u64 start = s.transient_next;
            isize const offset = isize(start % u64(region));
            if (offset + needed > region)
                start += u64(region - offset);

            if (start + u64(needed) - s.transient_freed > u64(region))
                return {}; // still in flight; the caller waits on an epoch and retries

            s.transient_next = start + u64(needed);
            return vulkan_descriptor_range{
                .offset = _transient_begin + isize(start % u64(region)),
                .size = needed,
                .transient = true,
            };
        });
}

void vulkan_descriptor_heap::on_epoch_advance(sg::epoch closed)
{
    if (_mapped == nullptr)
        return;
    _state.lock([&](heap_state& s)
                { s.checkpoints.push_back(checkpoint{.epoch_id = closed, .end_pos = s.transient_next}); });
}

void vulkan_descriptor_heap::on_epochs_completed(sg::epoch completed)
{
    if (_mapped == nullptr)
        return;
    _state.lock(
        [&](heap_state& s)
        {
            isize freed = 0;
            for (auto const& cp : s.checkpoints)
            {
                if (u64(cp.epoch_id) > u64(completed))
                    break;
                s.transient_freed = cp.end_pos;
                ++freed;
            }
            if (freed > 0)
                s.checkpoints.remove_at_range({.offset = 0, .size = freed});
        });
}
} // namespace sg::backend::vulkan
