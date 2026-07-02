#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/context.hh>

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
                   VkDebugUtilsMessengerEXT debug_messenger)
      : sg::context(sg::backend_kind::vulkan),
        _instance(instance),
        _physical_device(physical_device),
        _device(device),
        _queue(queue),
        _queue_family_index(queue_family_index),
        _debug_messenger(debug_messenger)
    {
    }

    ~vulkan_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

    // backend-typed API — prefer these when you already hold a vulkan_context

    [[nodiscard]] cc::result<std::unique_ptr<vulkan_command_list>> create_vulkan_command_list();
    [[nodiscard]] cc::result<vulkan_buffer_handle> create_vulkan_buffer(cc::isize size_in_bytes, sg::buffer_usage usage);
    void submit_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd);
    void drop_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd);

    // sg::context overrides — forward to the backend-typed methods above. The static_cast down is
    // sound: backends never mix.

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_vulkan_command_list());
    }

    [[nodiscard]] cc::result<sg::buffer_handle> create_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) override
    {
        return cc::result<sg::buffer_handle>(create_vulkan_buffer(size_in_bytes, usage));
    }

    void submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        submit_vulkan_command_list(std::unique_ptr<vulkan_command_list>(static_cast<vulkan_command_list*>(cmd.release())));
    }

    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        drop_vulkan_command_list(std::unique_ptr<vulkan_command_list>(static_cast<vulkan_command_list*>(cmd.release())));
    }

    void shutdown() override;

    // Helper: index of a device memory type satisfying `type_bits` (from a requirements mask) and all
    // of `properties`. Returns UINT32_MAX if none matches.
    [[nodiscard]] cc::u32 find_memory_type(cc::u32 type_bits, VkMemoryPropertyFlags properties) const;

    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physical_device = VK_NULL_HANDLE; // owned by the instance, not destroyed
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE; // owned by the device, not destroyed
    cc::u32 _queue_family_index = 0;
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
