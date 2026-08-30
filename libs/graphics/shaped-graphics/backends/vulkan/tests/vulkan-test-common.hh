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
/// Any validation message of warning severity or worse fails the running test, which is what makes the layer a gate
/// rather than log noise.
/// A test whose subject IS the bad input clears the callback for its duration.
[[nodiscard]] inline sg::context_handle make_context(sg::backend::vulkan::vulkan_config const& config
                                                     = {.enable_validation_layers = true})
{
    auto ctx = sg::create_vulkan_context(config);
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
