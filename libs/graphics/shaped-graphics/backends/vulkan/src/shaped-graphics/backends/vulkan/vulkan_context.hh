#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/command_list.hh> // complete type for the by-value unique_ptr<command_list> params
#include <shaped-graphics/context.hh>

namespace sg::backend::vulkan
{
/// Per-backend creation config for the Vulkan context (device selection, validation layers, ...).
/// Empty for now; grows as the backend is implemented. This is why context creation lives in the
/// backend rather than in sg — each backend takes its own config type.
struct vulkan_config
{
};

/// Vulkan implementation of sg::context. Backends derive directly from the sg interfaces — there
/// is no separate bridge layer — and have full access to the protected state. Smurf-named and
/// namespaced (sg::backend::vulkan) on purpose; see the sg coding guidelines. Members are defined
/// inline: backends favor readable, low-encapsulation code.
class vulkan_context final : public sg::context
{
public:
    vulkan_context() : sg::context(sg::backend_kind::vulkan, sg::thread_model::single_threaded) {}

    ~vulkan_context() override { shutdown(); } // ensures teardown before the base dtor's shut-down assert

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> create_command_list() override
    {
        return cc::error("vulkan backend not implemented yet");
    }

    [[nodiscard]] cc::result<sg::buffer_handle> create_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) override
    {
        return cc::error("vulkan backend not implemented yet");
    }

    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        CC_UNREACHABLE("vulkan backend not implemented yet");
    }

    // vulkan creates no command lists yet, so nothing is ever dropped; the hook exists for the ABC.
    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override {}

    // Epoch contract — stubbed until the backend is real. A backend need not track real in-flight
    // epochs; it just has to uphold the contract. These placeholders keep the sg interface complete.
    [[nodiscard]] sg::epoch current_epoch() const override { return sg::epoch::invalid; }
    [[nodiscard]] sg::epoch completed_epoch() const override { return sg::epoch::invalid; }
    void advance_epoch(cc::optional<int> allowed_in_flight) override
    {
        CC_UNREACHABLE("vulkan backend not implemented yet");
    }
    void advance_epoch_and_wait_for_idle() override { CC_UNREACHABLE("vulkan backend not implemented yet"); }
    void process_completed_epochs() override {}
    void wait_for_epoch(sg::epoch e) override { CC_UNREACHABLE("vulkan backend not implemented yet"); }
    void wait_for_next_inflight_epoch() override {}
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override { return false; }

    // No backend resources to release yet; the base shutdown() (sets the flag) is sufficient.
};
} // namespace sg::backend::vulkan

namespace sg
{
/// Creates a context on the Vulkan backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only a
/// caller that links the vulkan backend library sees this factory.
[[nodiscard]] cc::result<context_handle> create_vulkan_context(backend::vulkan::vulkan_config const& = {});
} // namespace sg
