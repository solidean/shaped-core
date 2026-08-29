#pragma once

#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_texture.hh>

/// Vulkan implementation of sg::raw_texture.
/// Holds the VkImage and its backing device-local VkDeviceMemory, always a dedicated allocation.
/// There is no layout tracking yet, so a texture is creatable but unusable in a command list until layout transitions land.
class sg::backend::vulkan::vulkan_texture final : public sg::raw_texture
{
public:
    vulkan_texture(vulkan_context& ctx,
                   sg::epoch created_in,
                   sg::texture_description const& desc,
                   VkImage image,
                   VkDeviceMemory memory)
      : sg::raw_texture(desc),
        _ctx(ctx),
        _creation_epoch(created_in),
        _image(image),
        _memory(memory),
        _access(vulkan_texture_access(subresource_extent_of(desc)))
    {
    }

    // Deferred deletion: hands the GPU handles and finalizers to the context, freed once the owning epoch retires.
    // Freeing them here could pull memory out from under a GPU still reading them.
    // Body in vulkan_texture.cc.
    ~vulkan_texture() override;

    /// Accumulate one declared access over `range`, seeding from the canonical layout on first touch.
    /// Thread-safe.
    void declare_access(sg::command_list_slot slot,
                        sg::subresource_range range,
                        sg::pipeline_stage_flags stages,
                        sg::access_flags access,
                        sg::texture_layout layout) const
    {
        _access.lock([&](vulkan_texture_access& a) { a.declare(slot, range, stages, access, layout); });
    }

    /// Test-and-set the per-op pending flag; true only the first time since the last flush.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.mark_pending_barrier(slot); });
    }

    /// Test-and-set the finalize-set flag; true only the first time for this slot.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.mark_recorded(slot); });
    }

    /// The per-subresource barriers satisfying everything declared for `slot` since the last flush.
    [[nodiscard]] cc::small_vector<vulkan_subresource_barrier, 4> flush_access(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.flush(slot); });
    }

    /// `slot`'s list was submitted; the last one out commits its layout, any earlier one reverts to canonical.
    [[nodiscard]] cc::small_vector<vulkan_subresource_barrier, 4> finalize_slot(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.finalize(slot); });
    }

    /// `slot`'s list was dropped; its work never runs, so the canonical layout is untouched.
    void discard_slot(sg::command_list_slot slot) const
    {
        _access.lock([&](vulkan_texture_access& a) { a.discard(slot); });
    }

    vulkan_context& _ctx;      // creating context — outlives this texture
    sg::epoch _creation_epoch; // epoch this texture was created in (immutable identity / diagnostics)
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;

    // Guarded because concurrent command lists may record against the same texture.
    // Mutable so a const handle can still track access: tracking is bookkeeping about the texture, not a change to it.
    mutable cc::mutex<vulkan_texture_access> _access;
};
