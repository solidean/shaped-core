#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context

// vulkan entry-point driver inside the sg API test binary (shaped-graphics-test).
// Vulkan has no guaranteed software device, so a context cannot be created on a driver-less headless host and the driver then SKIPs.
// When a device is present it invokes every sg::context_handle API test against it.
// Compiled only where the vulkan backend builds, so where the SDK is present.
//
// The backend is registered but its driver is disabled, and the two flags do different jobs while it is built out.
// Registering is what makes backends.cc build an alias per invocable, so `dev.py test "sg - <name>"` can run one API test against vulkan.
// nx::config::disabled keeps a full sweep out, because the recording paths vulkan has not reached yet abort rather than fail.
// Naming a test by its alias enables it deliberately, which is exactly the by-name run the build-out wants.
// Drop the disabled once no recording seam aborts.

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
void fail_on_validation_messages(sg::context_handle const& ctx)
{
    static_cast<vulkan::vulkan_context&>(*ctx).set_message_callback(
        [](vulkan::vulkan_message_severity severity, cc::string_view message)
        {
            if (severity <= vulkan::vulkan_message_severity::warning)
                CHECK(false).context(cc::format("vulkan validation: {}", message));
        });
}
} // namespace

TEST("sg vulkan backend", nx::config::disabled)
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("vulkan", ctx.value());
    }
}

static bool const sg_vulkan_registered = sg_test::register_backend("sg vulkan backend", "vulkan");
