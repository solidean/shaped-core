#pragma once

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

// Shared setup for the vulkan tier-2 suite (shaped-graphics-vulkan-test).
//
// Most tests are INVOCABLE_TESTs taking the one context the entry driver (vulkan-entry.cc) built for the whole run.
// Contexts are handed around as vulkan_context_handle, so a test that inspects backend guts needs no downcast.
// The helpers here are also for the few tests that need a context of their own: the context is their subject, or a vulkan_config knob is.
//
// Vulkan has no guaranteed software device — unlike dx12's WARP adapter — so a driver-less host cannot create a context at all.
// A test that finds none SKIPs rather than passing: a silent pass grows more dangerous the more the suite covers.

namespace sg::backend::vulkan::test
{
/// Fails whichever test provoked it on any validation message of warning severity or worse.
/// That is what makes the layer a gate rather than log noise.
/// A test that installs a callback of its own reinstalls this one before it returns, since the context outlives it.
inline void fail_on_validation_messages(vulkan_context& ctx)
{
    ctx.set_message_callback(
        [](vulkan_message_severity severity, cc::string_view message)
        {
            if (severity <= vulkan_message_severity::warning)
                CHECK(false).context(cc::format("vulkan validation: {}", message));
        });
}

/// A fresh context with validation, sync validation and the fail-on-validation listener, or nullptr on a host with no Vulkan device.
///
/// The caller takes `exclusive("vulkan-device")`.
/// Device creation and teardown are serialized process-wide (shaped-graphics/context/impl/device_lifecycle.hh), and a teardown slows with every other device still alive.
///
/// Synchronization validation is forced on here rather than defaulted, so a caller passing its own config still gets it.
/// It is the only check that sees a hazard between two submissions.
[[nodiscard]] inline vulkan_context_handle make_context(vulkan_config const& config = {.enable_validation_layers = true})
{
    auto sync_config = config;
    sync_config.enable_sync_validation = true;

    auto ctx = sg::create_vulkan_context(sync_config);
    if (ctx.has_error())
        return nullptr;

    auto typed = std::static_pointer_cast<vulkan_context>(ctx.value());
    fail_on_validation_messages(*typed);
    return typed;
}
} // namespace sg::backend::vulkan::test
