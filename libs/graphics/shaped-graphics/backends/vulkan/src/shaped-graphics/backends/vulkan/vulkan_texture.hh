#pragma once

#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh> // ctx_acquire_completion_group
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg::backend::vulkan
{
/// Mints the process-unique stamp behind vulkan_texture::_identity.
[[nodiscard]] u64 next_texture_identity();
} // namespace sg::backend::vulkan

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
                   VkDeviceMemory memory,
                   bool owns_image = true)
      : sg::raw_texture(desc),
        _ctx(ctx),
        _creation_epoch(created_in),
        _image(image),
        _memory(memory),
        _owns_image(owns_image),
        _access(vulkan_texture_access(subresource_extent_of(desc), desc.resolved_initial_layout()))
    {
        // A timeline only where a transfer is even possible — see the same rule on vulkan_buffer.
        if (desc.usage.has(sg::texture_usage::copy_dst))
            _upload_group = ctx_acquire_completion_group(ctx);
        if (desc.usage.has(sg::texture_usage::copy_src))
            _download_group = ctx_acquire_completion_group(ctx);
    }

    // Deferred deletion: hands the GPU handles and finalizers to the context, freed once the owning epoch retires.
    // Freeing them here could pull memory out from under a GPU still reading them.
    // Body in vulkan_texture.cc.
    ~vulkan_texture() override;

    /// Stages the GPU handles for deferred deletion and clears them; idempotent.
    /// Both expiry and destruction run it, and whichever comes first owns the release.
    void release_storage() const;

protected:
    // A transient texture expires when its epoch advances — dedicated storage today, but the contract is the same.
    void on_expired() const override;

public:
    /// Accumulate one declared access over `range`, entering an untouched box at `layout`.
    /// Thread-safe.
    void declare_access(sg::command_list_slot slot,
                        sg::subresource_range range,
                        sg::pipeline_stage_flags stages,
                        sg::access_flags access,
                        sg::texture_layout layout) const
    {
        _access.lock([&](vulkan_texture_access& a) { a.declare(slot, range, stages, access, layout); });
    }

    /// The layout `range` is in as of the last submitted command list.
    /// Thread-safe.
    [[nodiscard]] sg::texture_layout current_layout_of(sg::subresource_range range) const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.current_layout_of(range); });
    }

    /// Claims the one-time UNDEFINED -> resting transition for the caller, or false if someone already has.
    /// Thread-safe.
    /// Called from a submit path, so the claimer is the first to actually run.
    [[nodiscard]] bool claim_initial_transition() const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.claim_initial_transition(); });
    }

    /// Whether the image is still in the layout vkCreateImage left it in.
    /// A hint at record time: another list may claim the transition before this one submits, which is why the set it
    /// feeds is tentative and claim_initial_transition is what decides.
    [[nodiscard]] bool needs_initial_transition() const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.needs_initial_transition(); });
    }

    /// The layout this texture starts in before anything has used it — what the initial transition targets.
    [[nodiscard]] sg::texture_layout resting_layout() const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.resting_layout(); });
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

    /// `slot`'s list was submitted: its layouts become the current ones, and the barriers returned are the entry
    /// transitions the caller prepends to that same submit.
    [[nodiscard]] cc::small_vector<vulkan_subresource_barrier, 4> finalize_slot(sg::command_list_slot slot) const
    {
        return _access.lock([&](vulkan_texture_access& a) { return a.finalize(slot); });
    }

    /// `slot`'s list was dropped; its work never runs, so the current layout is untouched.
    void discard_slot(sg::command_list_slot slot) const
    {
        _access.lock([&](vulkan_texture_access& a) { a.discard(slot); });
    }

    vulkan_context& _ctx;      // creating context — outlives this texture
    sg::epoch _creation_epoch; // epoch this texture was created in (immutable identity / diagnostics)

    /// Process-unique for the life of the context, and the key the image view cache is built on.
    /// The address is NOT a usable identity there: it is reusable the moment this object dies, while the cache
    /// entries naming it survive until the owning epoch retires — see vulkan_view_desc.hh.
    u64 _identity = next_texture_identity();
    // Mutable because release_storage() is const: expiry is a lifetime event on a const handle.
    mutable VkImage _image = VK_NULL_HANDLE;
    mutable VkDeviceMemory _memory = VK_NULL_HANDLE;

    /// The completion timelines and cross-queue stamps, exactly as on vulkan_buffer — see the notes there.
    vulkan_completion_group_handle _upload_group;
    vulkan_completion_group_handle _download_group;
    mutable cc::atomic<u64> _pending_async_upload_value = {0};
    mutable cc::atomic<u64> _last_used_submission_token = {0};
    mutable cc::atomic<u64> _pending_async_download_value = {0};
    mutable cc::atomic<u64> _pending_stream_copy_value = {0};
    mutable cc::atomic<u64> _pending_stream_download_value = {0};

    /// False for a *borrowed* image, which something else owns — a swapchain's, whose images belong to the
    /// VkSwapchainKHR and are destroyed with it.
    /// The wrapper still tracks access, which is what lets a back buffer flow through the ordinary barrier path
    /// rather than being special-cased at every use.
    bool _owns_image = true;

    // Guarded because concurrent command lists may record against the same texture.
    // Mutable so a const handle can still track access: tracking is bookkeeping about the texture, not a change to it.
    mutable cc::mutex<vulkan_texture_access> _access;
};
