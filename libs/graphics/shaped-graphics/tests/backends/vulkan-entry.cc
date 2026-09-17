#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/thread_scope.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context

// vulkan entry-point driver inside the sg API test binary (shaped-graphics-test).
// Vulkan has no guaranteed software device, so a context cannot be created on a driver-less headless host and the driver then SKIPs.
// When a device is present it invokes every sg::context_handle API test against it.
// Compiled only where the vulkan backend builds, so where the SDK is present.
//
// The driver ran `nx::config::disabled` throughout the build-out, because a seam vulkan had not reached yet aborted
// rather than failing — which turns a clean skip into a crash that takes the suite with it.
// No seam aborts any more, so it is enabled and the whole tier-1 suite sweeps against vulkan like dx12's.

namespace
{
namespace vulkan = sg::backend::vulkan;

// Fails whichever test provoked it on any validation message of warning severity or worse.
// Without this a validation error is a line in the log nobody reads, and the run stays green — which is what the dx12
// backend did for ~680 of them before it grew the same listener.
// The Khronos layer is stricter than D3D12's, so this is the primary oracle while the backend is written.
// Per-context rather than thread-scoped, unlike dx12's: a Vulkan messenger belongs to one instance and delivers only
// that instance's messages.
// See vulkan_context::set_message_callback.
//
// A message raised where no test is installed lands on this driver.
// Synchronization validation can raise one inside a later, unrelated submission when it re-checks a deferred one.
// The context is the driver's own, so the captured driver is released with it, before the driver ends.
void fail_on_validation_messages(sg::context_handle const& ctx)
{
    auto& vk = static_cast<vulkan::vulkan_context&>(*ctx);
    vk.set_message_callback(
        [&vk, driver = nx::capture_current_test()](vulkan::vulkan_message_severity severity, cc::string_view message)
        {
            if (severity > vulkan::vulkan_message_severity::warning)
                return;

            // A hazard between two copies is only diagnosable with their ranges and order, which the message lacks.
            auto const windows = message.contains("_AFTER_WRITE") ? vk.describe_recent_transfer_windows() : cc::string();
            nx::with_fallback_test(
                driver, [&] { CHECK(false).context(cc::format("vulkan validation: {}\n{}", message, windows)); });
        });
}
} // namespace

// No exclusion tags, for the reason dx12-entry.cc gives.
ASYNC_TEST("sg vulkan backend")
{
    // Synchronization validation is on for the whole tier-1 sweep: it is the only oracle that sees a hazard between
    // two submissions, which is what the cross-list and cross-queue ordering work is about.
    // It needs a validation layer from SDK 1.4.350 or newer: older ones report WRITE_RACING_READ between queues that
    // never raced — docs/bugs-external/vulkan-syncval-wait-before-signal-false-race.
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true, .enable_sync_validation = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
    {
        fail_on_validation_messages(ctx.value());
        co_await nx::async_invoke_tests_in_sequence("vulkan", ctx.value());

        // A device loss during our own tests is a defect, not an environment quirk to tolerate.
        // Vulkan has no equivalent of dx12's poll, so this sees only a loss some operation already noticed --
        // which every submitting test does.
        CHECK(!ctx.value()->is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}",
                                ctx.value()->device_loss_reason()));
    }
}

// The whole sweep again under a browser's rules; see the dx12 never-block driver.
// Under --thorough only: the dx12 one already proves the property by default on a software adapter, and a vulkan device here is a hardware one.
ASYNC_TEST("sg vulkan never-block backend")
{
    if (!nx::is_thorough())
        SKIP("the dx12 never-block driver covers the default run on WARP; this one runs under --thorough");

    auto ctx = sg::create_vulkan_context(
        {.enable_validation_layers = true, .enable_sync_validation = true, .execution = sg::execution_model::never_block});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
    {
        fail_on_validation_messages(ctx.value());
        co_await nx::async_invoke_tests_in_sequence("vulkan-never-block", ctx.value());
        CHECK(!ctx.value()->is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}",
                                ctx.value()->device_loss_reason()));
    }
}

static bool const sg_vulkan_registered = sg_test::register_backend("sg vulkan backend", "vulkan");
static bool const sg_vulkan_never_block_registered
    = sg_test::register_backend("sg vulkan never-block backend", "vulkan-never-block");
static bool const sg_vulkan_factory_registered
    = sg_test::register_context_factory("vulkan", [] { return sg::create_vulkan_context({}); });
