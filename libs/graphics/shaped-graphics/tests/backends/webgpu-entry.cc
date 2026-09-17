#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/thread_scope.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh> // sg::request_webgpu_context

// WebGPU entry-point driver inside the sg API test binary (shaped-graphics-test).
// Compiled only for the Emscripten WebGPU presets; under node the `webgpu` package installs navigator.gpu, and a runtime without it SKIPs.
// The sweep runs under both node (Dawn) and deno (wgpu), which between them are the two browser WebGPU implementations.
// It runs on the main thread, and its invoked tests inherit that home, because in a threaded wasm build WebGPU objects exist only on the thread that requested the device.

namespace
{
namespace webgpu = sg::backend::webgpu;

// Fails whichever test provoked it on any WebGPU validation or out-of-memory error.
// WebGPU reports them from a later task than the call that caused them, so an error the running test did not provoke lands on the driver instead.
void fail_on_webgpu_errors(sg::context_handle const& ctx)
{
    static_cast<webgpu::webgpu_context&>(*ctx).set_message_callback(
        [driver = nx::capture_current_test()](WGPUErrorType type, cc::string_view message)
        {
            auto const kind = type == WGPUErrorType_OutOfMemory ? "out of memory" : "validation";
            nx::with_fallback_test(driver, [&] { CHECK(false).context(cc::format("webgpu {}: {}", kind, message)); });
        });
}
} // namespace

ASYNC_TEST("sg webgpu backend", main_thread)
{
    auto const requested = co_await cc::async_as_result(sg::request_webgpu_context());
    if (requested.has_error())
        SKIP("no webgpu adapter");
    else
    {
        auto const ctx = requested.value();
        fail_on_webgpu_errors(ctx);
        co_await nx::async_invoke_tests_in_sequence("webgpu", nx::invocation_options{.inherit_home = true}, ctx);

        // Nothing here can wait, so what the tests left running is awaited before the context goes.
        co_await cc::async_settled(ctx->backlog.settled());
        co_await cc::async_settled(ctx->idle_completion());

        CHECK(!ctx->is_device_lost())
            .context(
                cc::format("the device was lost while running this binary's GPU tests: {}", ctx->device_loss_reason()));
        static_cast<webgpu::webgpu_context&>(*ctx).set_message_callback({});
        ctx->shutdown();
    }
}

static bool const sg_webgpu_registered = sg_test::register_backend("sg webgpu backend", "webgpu");
