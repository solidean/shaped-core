#pragma once

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

// Shared setup for the vulkan tier-2 suite.
//
// Vulkan has no guaranteed software device — unlike dx12's WARP adapter — so a driver-less host cannot create a
// context at all, and a test that finds none SKIPs rather than passing.
// A silent pass is the outcome worth avoiding: it grows more dangerous the more the suite covers.

namespace sg::backend::vulkan::test
{
/// A fresh context with the fail-on-validation listener installed, or nullptr on a host with no Vulkan device.
///
/// A test that creates a context takes `exclusive("vulkan-device")`.
/// Device creation and teardown are serialized process-wide (vulkan_driver_lock.hh), and a teardown slows with every
/// other device still alive, so thirty tests creating contexts at once run twice as long as the same thirty in turn.
///
/// Any validation message of warning severity or worse fails the running test, which is what makes the layer a gate
/// rather than log noise.
/// A test whose subject IS the bad input clears the callback for its duration.
/// Synchronization validation is forced on here rather than defaulted, so a caller passing its own config still gets
/// it — it is the only check that sees a hazard between two submissions.
[[nodiscard]] inline sg::context_handle make_context(sg::backend::vulkan::vulkan_config const& config
                                                     = {.enable_validation_layers = true})
{
    auto sync_config = config;
    sync_config.enable_sync_validation = true;

    auto ctx = sg::create_vulkan_context(sync_config);
    if (ctx.has_error())
        return nullptr;

    static_cast<vulkan_context&>(*ctx.value())
        .set_message_callback(
            [](vulkan_message_severity severity, cc::string_view message)
            {
                if (severity <= vulkan_message_severity::warning)
                    CHECK(false).context(cc::format("vulkan validation: {}", message));
            });
    return ctx.value();
}
} // namespace sg::backend::vulkan::test
