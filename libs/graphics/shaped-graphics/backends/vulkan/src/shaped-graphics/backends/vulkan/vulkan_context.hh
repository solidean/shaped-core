#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_epoch.hh>
#include <shaped-graphics/context.hh>

#include <atomic>
#include <memory>

namespace sg::backend::vulkan
{
/// Per-backend creation config for the Vulkan context. The two flags are independent. This is why
/// context creation lives in the backend rather than in sg — each backend takes its own config type.
struct vulkan_config
{
    /// Enable the Khronos validation layer plus a debug messenger that routes messages to stderr.
    /// Best-effort — skipped if the layer / VK_EXT_debug_utils isn't installed.
    bool enable_validation_layers = false;

    /// Prefer a software (CPU) physical device, e.g. lavapipe. The closest analog to dx12's WARP
    /// adapter, but a *preference*: unlike WARP it falls back to hardware when no CPU device is
    /// present (Vulkan has no guaranteed software device).
    bool prefer_software_device = false;
};

/// Vulkan implementation of sg::context. Backends derive directly from the sg interfaces — there is
/// no separate bridge layer — and have full access to the protected state. The sg::context virtuals
/// are thin forwarders to the backend-typed create_vulkan_* methods — prefer those when you hold a
/// vulkan_context. Members are public: backends favor readable, low-encapsulation code. Bodies live
/// in vulkan_context*.cc / vulkan_command_list.cc / vulkan_buffer.cc.
class vulkan_context final : public sg::context
{
public:
    vulkan_context(VkInstance instance,
                   VkPhysicalDevice physical_device,
                   VkDevice device,
                   VkQueue queue,
                   cc::u32 queue_family_index,
                   VkSemaphore epoch_timeline,
                   VkSemaphore submission_timeline,
                   VkDebugUtilsMessengerEXT debug_messenger)
      : sg::context(sg::backend_kind::vulkan, sg::thread_model::multi_threaded),
        _instance(instance),
        _physical_device(physical_device),
        _device(device),
        _queue(queue),
        _queue_family_index(queue_family_index),
        _epoch_timeline(epoch_timeline),
        _submission_timeline(submission_timeline),
        _debug_messenger(debug_messenger)
    {
    }

    ~vulkan_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

    // backend-typed API — prefer these when you already hold a vulkan_context

    [[nodiscard]] cc::result<std::unique_ptr<vulkan_command_list>> create_vulkan_command_list();
    [[nodiscard]] cc::result<vulkan_buffer_handle> create_vulkan_buffer(cc::isize size_in_bytes,
                                                                        sg::buffer_usage usage,
                                                                        sg::allocation_info const& alloc);
    sg::submission_token submit_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd);
    void drop_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd);

    // sg::context overrides — forward to the backend-typed methods above. The static_cast down is
    // sound: backends never mix.

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_vulkan_command_list());
    }

    [[nodiscard]] cc::result<sg::buffer_handle> create_buffer(cc::isize size_in_bytes,
                                                              sg::buffer_usage usage,
                                                              sg::allocation_info const& alloc) override
    {
        return cc::result<sg::buffer_handle>(create_vulkan_buffer(size_in_bytes, usage, alloc));
    }

    [[nodiscard]] cc::result<sg::memory_heap_handle> create_memory_heap(cc::isize) override
    {
        CC_UNREACHABLE("vulkan memory_heap creation is not implemented yet");
    }

    // Bind path (binding_layout / compute_pipeline / binding_group) — not implemented yet.
    [[nodiscard]] cc::result<sg::binding_layout_handle> create_binding_layout(cc::span<sg::binding const>,
                                                                              sg::lifetime_scope) override
    {
        CC_UNREACHABLE("vulkan binding_layout creation is not implemented yet");
    }
    [[nodiscard]] cc::result<sg::compute_pipeline_handle> create_compute_pipeline(sg::compute_pipeline_description const&,
                                                                                  sg::lifetime_scope) override
    {
        CC_UNREACHABLE("vulkan compute_pipeline creation is not implemented yet");
    }
    [[nodiscard]] cc::result<sg::binding_group_handle> create_binding_group(sg::binding_layout_handle,
                                                                            cc::span<sg::named_view const>,
                                                                            sg::lifetime_scope) override
    {
        CC_UNREACHABLE("vulkan binding_group creation is not implemented yet");
    }

    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        return submit_vulkan_command_list(
            std::unique_ptr<vulkan_command_list>(static_cast<vulkan_command_list*>(cmd.release())));
    }

    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        drop_vulkan_command_list(std::unique_ptr<vulkan_command_list>(static_cast<vulkan_command_list*>(cmd.release())));
    }

    // Async upload (ctx.upload) — not implemented yet.
    void async_upload_bytes_to_buffer(sg::buffer_handle, cc::pinned_data<cc::byte const>, cc::isize) override
    {
        CC_UNREACHABLE("vulkan async upload is not implemented yet");
    }

    // Deferred deletion: a refcount-zero GPU resource, staged for the current epoch and freed once
    // that epoch retires. Called from ~vulkan_buffer; safe to call from any thread.
    void schedule_deferred_deletion(vulkan_expiring_resource expiring);

    // Epoch contract — bodies in vulkan_epoch.cc. These return sg vocabulary types (no backend-typed
    // variant needed), so the whole body lives in the override. Realized on a pair of timeline
    // semaphores: the epoch timeline gates reclamation, the submission timeline answers per-list queries.

    [[nodiscard]] sg::epoch current_epoch() const override { return _current_epoch; }
    [[nodiscard]] sg::epoch completed_epoch() const override;
    void advance_epoch(cc::optional<int> allowed_in_flight) override;
    void advance_epoch_and_wait_for_idle() override { advance_epoch(0); }
    void process_completed_epochs() override;
    void wait_for_epoch(sg::epoch e) override;
    void wait_for_next_inflight_epoch() override;
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override;

    void shutdown() override;

    // Helper: index of a device memory type satisfying `type_bits` (from a requirements mask) and all
    // of `properties`. Returns UINT32_MAX if none matches.
    [[nodiscard]] cc::u32 find_memory_type(cc::u32 type_bits, VkMemoryPropertyFlags properties) const;

    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physical_device = VK_NULL_HANDLE; // owned by the instance, not destroyed
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE; // owned by the device, not destroyed
    cc::u32 _queue_family_index = 0;

    // Epoch machinery. The epoch timeline is signaled with the epoch value at the end of each epoch;
    // the submission timeline is a per-command-list value on the same queue (Vulkan's analog of dx12's
    // two direct-queue fences). Both are VK_SEMAPHORE_TYPE_TIMELINE, so completion is a counter read.
    VkSemaphore _epoch_timeline = VK_NULL_HANDLE;
    VkSemaphore _submission_timeline = VK_NULL_HANDLE;

    // Written only by advance (externally synchronized), read concurrently by create/submit/drop.
    sg::epoch _current_epoch = sg::epoch::first;

    // create / submit / drop are thread-safe, so the shared bookkeeping they touch is synchronized:
    //  - _open_command_lists: bumped per create, dropped per submit/drop — a lock-free counter.
    //  - _next_submission: the next completion token; guarded together with the vkQueueSubmit + signal
    //    in submit so token order == queue/signal order (out-of-order signals would break completion).
    //  - _command_pools: the command-pool pool (see vulkan_command_pool_set).
    std::atomic<int> _open_command_lists = 0; // must reach 0 before advance — lists cannot span epochs
    cc::mutex<sg::submission_token> _next_submission{sg::submission_token::first};
    cc::mutex<vulkan_command_pool_set> _command_pools;

    cc::mutex<vulkan_epoch_state> _epoch_state;

    VkDebugUtilsMessengerEXT _debug_messenger = VK_NULL_HANDLE; // VK_NULL_HANDLE when validation is off
};
} // namespace sg::backend::vulkan

namespace sg
{
/// Creates a context on the Vulkan backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only a
/// caller that links the vulkan backend library sees this factory. Returns an error (never asserts)
/// on environment failure — no loader, no device, device creation refused, etc.
[[nodiscard]] cc::result<context_handle> create_vulkan_context(backend::vulkan::vulkan_config const& config = {});
} // namespace sg
