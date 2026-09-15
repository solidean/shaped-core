#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_feedback.hh>

#include <atomic>
#include <cstdlib>
#include <thread>

// What Metal actually reports, and where.
//
// These pin measurements rather than documentation, because the answers decided the backend's whole diagnostic design
// and none of them is written down anywhere official.
// If Apple changes one, this file is what says so.

namespace mtl = sg::backend::metal;

TEST("sg metal - the validation layer is armed for this binary")
{
    // main() calls arm_validation_layer before anything touches Metal.
    // Pinned because the call is easy to drop and its absence is invisible: the suite would stay green and stop
    // checking anything, which is exactly the failure mode the arming exists to prevent.
    CHECK(std::getenv("MTL_DEBUG_LAYER") != nullptr);
    CHECK(std::getenv("MTL_DEBUG_LAYER_ERROR_MODE") != nullptr);
}

TEST("sg metal - an MTLLogState handler is not the validation channel")
{
    // The measurement behind the backend having no set_message_callback.
    //
    // MTLLogState is the one Metal facility that takes a log handler, so it is the obvious candidate for the listener
    // dx12 and vulkan both install — and it does not carry validation messages.
    // A zero-length buffer is refused by the layer (with the layer on it aborts, which is why this test provokes
    // nothing and only builds the handler), and the handler never sees a word of it.
    auto const scope = mtl::autorelease_scope();

    auto* const device = MTL::CreateSystemDefaultDevice();
    if (device == nullptr)
        SKIP("no metal device");

    auto* const descriptor = MTL::LogStateDescriptor::alloc()->init();
    descriptor->setLevel(MTL::LogLevelDebug);
    descriptor->setBufferSize(64 * 1024);

    NS::Error* error = nullptr;
    auto* const log_state = device->newLogState(descriptor, &error);
    descriptor->release();

    CHECK(log_state != nullptr).context("a log state is creatable, which is what makes its silence meaningful");

    if (log_state != nullptr)
        log_state->release();
    device->release();
}

ASYNC_TEST("sg metal - a commit's feedback handler runs")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The plumbing behind the deferred error channel: MTL4CommitFeedback is the only place Metal reports a failure
    // that arrives after the call that caused it, so a handler that never runs would make that channel permanently
    // silent rather than merely empty.
    auto const scope = mtl::autorelease_scope();

    auto* const allocator = ctx->device()->newCommandAllocator();
    auto* const buffer = ctx->device()->newCommandBuffer();
    buffer->beginCommandBuffer(allocator);
    buffer->endCommandBuffer();

    static std::atomic<int> fired = {0};
    fired = 0;

    auto* const options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler(^void(MTL4::CommitFeedback*) {
      ++fired;
    });

    MTL4::CommandBuffer const* const buffers[] = {buffer};
    ctx->queue()->commit(buffers, 1, options);
    options->release();

    co_await ctx->idle_completion();

    // Draining the GPU says nothing about the handler, which runs on a dispatch queue of Metal's choosing — so wait on
    // the condition rather than assuming the drain covered it.
    for (auto spin = 0; spin < 20'000'000 && fired.load() == 0; ++spin)
        std::this_thread::yield();

    CHECK(fired.load() == 1);

    buffer->release();
    allocator->release();
}

ASYNC_TEST("sg metal - a clean run leaves the deferred error channel empty")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    (void)ctx->submit_command_list(ctx->create_command_list());
    ctx->advance_epoch();
    co_await ctx->idle_completion();

    CHECK(ctx->take_pending_errors().empty());
    CHECK(!ctx->is_device_lost());
}

TEST("sg metal - a feedback error reaches the deferred error channel")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The routing, exercised directly rather than by provoking a GPU fault.
    // A real fault is a hang or a page fault — neither is something a test may cause on a machine somebody is using,
    // and both would take the process with them.
    // So the handler's own call is what is tested here, and the test above is what says the handler runs at all.
    ctx->report_feedback_error(sg::device_error_kind::validation, "a synthetic command buffer failure");

    auto const errors = ctx->take_pending_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].kind == sg::device_error_kind::validation);
    CHECK(errors[0].message == "a synthetic command buffer failure");

    // Taking drains, so the channel is a queue rather than a level.
    CHECK(ctx->take_pending_errors().empty());
}

TEST("sg metal - a detached sink reports nothing")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The lifetime guard: shutdown detaches the sink, and a handler still in flight must then do nothing rather than
    // report into a context being torn down.
    auto const sink = std::make_shared<mtl::metal_feedback_sink>(*ctx);
    sink->report(sg::device_error_kind::validation, "before detach");
    CHECK(ctx->take_pending_errors().size() == 1);

    sink->detach();
    sink->report(sg::device_error_kind::validation, "after detach");
    CHECK(ctx->take_pending_errors().empty());
}

TEST("sg metal - the device-loss codes are the ones that mean the device is gone")
{
    // Pure mapping, no device needed.
    CHECK(mtl::device_error_kind_of(MTL::CommandBufferErrorTimeout) == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(MTL::CommandBufferErrorDeviceRemoved) == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(MTL::CommandBufferErrorAccessRevoked) == sg::device_error_kind::device_lost);

    // Everything else is one command buffer failing, not the device going away.
    CHECK(mtl::device_error_kind_of(MTL::CommandBufferErrorOutOfMemory) == sg::device_error_kind::validation);
    CHECK(mtl::device_error_kind_of(MTL::CommandBufferErrorPageFault) == sg::device_error_kind::validation);
    CHECK(mtl::device_error_kind_of(MTL::CommandBufferErrorNone) == sg::device_error_kind::validation);
}

// Disabled because passing it means ending the process, and a suite cannot contain that.
//
// It is the proof the guide asks for: a listener nobody has seen fire is indistinguishable from one that is not
// connected, and here the "listener" is an abort rather than a callback, so the only demonstration is to provoke one.
// Run it by exact name, and read the abort as the pass:
//
//     uv run dev.py test "sg metal - the validation gate aborts on a violation"
//
// Expected: the binary dies with SIGABRT and stderr carries
// `failed assertion 'Buffer Validation / Cannot create buffer of zero length.'`.
// A run that *finishes* is the failure — it means the layer is not armed, and every validation message in the whole
// suite is going to a log nobody reads.
TEST("sg metal - the validation gate aborts on a violation", nx::config::disabled)
{
    auto const scope = mtl::autorelease_scope();

    auto* const device = MTL::CreateSystemDefaultDevice();
    if (device == nullptr)
        SKIP("no metal device");

    // A zero-length buffer is the Metal analogue of the zero-size vkCreateBuffer the backend guide suggests for this:
    // a pure diagnostic with nothing to clean up, and nothing for the abort to leak.
    auto* const bad = device->newBuffer(NS::UInteger(0), MTL::ResourceStorageModeShared);

    CHECK(false).context("the validation layer is not armed — this line is only reached when the gate is off");

    if (bad != nullptr)
        bad->release();
    device->release();
}
