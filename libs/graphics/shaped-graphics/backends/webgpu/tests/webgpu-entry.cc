#include "webgpu-test-common.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/thread_scope.hh>

// Entry-point driver for the WebGPU backend suite (shaped-graphics-webgpu-test).
// It requests ONE context and invokes every invocable in the binary against it, so the suite pays for one device rather than one per test.
// A runtime without WebGPU — no navigator.gpu, or no adapter — SKIPs.
// It runs on the main thread, and its invoked tests inherit that home, because in a threaded wasm build WebGPU objects exist only on the thread that requested the device.
//
// The invocables run serially on this context, so each leaves it clean: every list submitted or dropped, and every async it started settled.

namespace
{
namespace webgpu = sg::backend::webgpu;

constexpr char const* driver = "sg webgpu backend - device";
} // namespace

ASYNC_TEST("sg webgpu backend - device", main_thread)
{
    auto const requested = co_await cc::async_as_result(sg::request_webgpu_context());
    if (requested.has_error())
        SKIP("no webgpu adapter");
    else
    {
        auto const ctx = std::static_pointer_cast<webgpu::webgpu_context>(requested.value());

        // WebGPU reports a validation error from a later task than the call that caused it, so one the running test did not provoke lands on this driver.
        ctx->set_message_callback(
            [capture = nx::capture_current_test()](WGPUErrorType, cc::string_view message)
            { nx::with_fallback_test(capture, [&] { CHECK(false).context(cc::format("webgpu error: {}", message)); }); });

        co_await nx::async_invoke_tests_in_sequence("device", nx::invocation_options{.inherit_home = true}, ctx);

        co_await cc::async_settled(ctx->backlog.settled());
        co_await cc::async_settled(ctx->idle_completion());
        CHECK(!ctx->is_device_lost())
            .context(
                cc::format("the device was lost while running this binary's GPU tests: {}", ctx->device_loss_reason()));
        ctx->set_message_callback({});
        ctx->shutdown();
    }
}

// One alias per invocable, so `dev.py test "sg webgpu - <name>"` still selects that one test.
NX_TEST_SETUP(nx::setup& s)
{
    auto const* const entry = s.find_test(driver);
    if (entry == nullptr)
        return;

    for (auto const* t : s.invocables_with<webgpu::webgpu_context_handle>())
    {
        cc::vector<nx::alias_fragment> fragments;
        fragments.push_back(nx::alias_fragment{.driver = entry, .section_path = {"device", t->name}});
        s.define_alias(t->name, cc::move(fragments));
    }
}
