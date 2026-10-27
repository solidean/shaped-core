#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_feedback.hh>
#include <shaped-graphics/exceptions.hh>

#include <atomic>
#include <cstdlib>

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

    // **A host that cannot make one cannot make this measurement**, so it skips rather than failing.
    // What makes the silence meaningful is a log state existing to be silent; where none can be created there is
    // nothing to conclude either way.
    // CI is exactly that host — a GitHub runner's paravirtualised GPU creates a device and refuses a log state — which
    // also means this measurement is a developer-machine one, like every other device test in this suite.
    if (log_state == nullptr)
    {
        device->release();
        SKIP("this host creates no MTLLogState, so its silence proves nothing");
    }

    // Creation succeeded, so Metal reported nothing alongside it.
    CHECK(error == nullptr).context("a log state was created, so nothing should have been reported making it");

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
        cc::this_thread_yield();

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

namespace
{
/// An NSError as a failed commit's feedback carries it, autoreleased.
NS::Error* commit_error(char const* domain, NS::Integer code)
{
    return NS::Error::error(NS::String::string(domain, NS::UTF8StringEncoding), code, nullptr);
}
} // namespace

TEST("sg metal - the device-loss codes are the ones that mean the device is gone")
{
    // Pure mapping, no device needed.
    auto const scope = mtl::autorelease_scope();

    // **The domain decides what a code means**, and an MTL4 commit reports in the queue's domain.
    // There 1 is a timeout, which is also what a GPU page fault arrives as.
    // In the classic domain 1 is an internal error.
    // Reading the one through the other's enum is what once filed every lost device as an ordinary failure.
    auto const* const mtl4 = "MTL4CommandQueueErrorDomain";
    CHECK(mtl::device_error_kind_of(commit_error(mtl4, MTL4::CommandQueueErrorTimeout))
          == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(commit_error(mtl4, MTL4::CommandQueueErrorNotPermitted))
          == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(commit_error(mtl4, MTL4::CommandQueueErrorDeviceRemoved))
          == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(commit_error(mtl4, MTL4::CommandQueueErrorAccessRevoked))
          == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(commit_error(mtl4, MTL4::CommandQueueErrorOutOfMemory))
          == sg::device_error_kind::validation);
    CHECK(mtl::device_error_kind_of(commit_error(mtl4, MTL4::CommandQueueErrorInternal))
          == sg::device_error_kind::validation);

    auto const* const classic = "MTLCommandBufferErrorDomain";
    CHECK(mtl::device_error_kind_of(commit_error(classic, NS::Integer(MTL::CommandBufferErrorTimeout)))
          == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(commit_error(classic, NS::Integer(MTL::CommandBufferErrorDeviceRemoved)))
          == sg::device_error_kind::device_lost);
    CHECK(mtl::device_error_kind_of(commit_error(classic, NS::Integer(MTL::CommandBufferErrorAccessRevoked)))
          == sg::device_error_kind::device_lost);

    // Everything else is one command buffer failing, not the device going away.
    CHECK(mtl::device_error_kind_of(commit_error(classic, NS::Integer(MTL::CommandBufferErrorInternal)))
          == sg::device_error_kind::validation);
    CHECK(mtl::device_error_kind_of(commit_error(classic, NS::Integer(MTL::CommandBufferErrorPageFault)))
          == sg::device_error_kind::validation);
    CHECK(mtl::device_error_kind_of(commit_error("SomeOtherDomain", 1)) == sg::device_error_kind::validation);
    CHECK(mtl::device_error_kind_of(nullptr) == sg::device_error_kind::validation);
}

ASYNC_TEST("sg metal - a lost device settles the waits its stopped queue strands")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // **What a real loss leaves behind, minus the fault.**
    // After a GPU fault the system restarts the GPU and refuses the process's submissions, so no queued signal ever
    // runs again and every timeline stops where it was.
    // A queue wait on an event nobody signals stops this queue the same way, and without touching the GPU's health.
    auto* const blocker = ctx->device()->newSharedEvent();
    REQUIRE(blocker != nullptr);
    ctx->queue()->wait(blocker, 1);

    // A command list stranded behind the wait puts an allocator in the closing epoch.
    // The loss retires that epoch while the buffer is still pending, which is why a lost device leaves the allocator
    // alone; the validation layer does not flag a reset of this empty list's allocator, so this pins the path only.
    ctx->submit_command_list(ctx->create_command_list());
    auto const closed = ctx->current_epoch();
    ctx->advance_epoch();

    // Behind everything above, so the test can see the queue finish once it lets it go.
    auto* const drained = ctx->device()->newSharedEvent();
    REQUIRE(drained != nullptr);
    ctx->queue()->signalEvent(drained, 1);

    // Minted before the loss, so the completion waiter is armed for a value the queue will never reach.
    auto const stranded = ctx->epoch_completion(closed);
    CHECK(!stranded->is_ready());

    // The feedback handler's own route, with the error a page fault really reports.
    // Scoped, because an autorelease pool belongs to a thread and the awaits below may resume on another.
    {
        auto const scope = mtl::autorelease_scope();
        auto* const error = commit_error("MTL4CommandQueueErrorDomain", MTL4::CommandQueueErrorTimeout);
        ctx->feedback_sink()->report(mtl::device_error_kind_of(error), mtl::describe_error(error, "a simulated fault"));
    }

    // Each of these hung before the loss was recognised.
    // The error went to the channel as an ordinary failure, and nothing ever raised the timelines the waits parked on.
    co_await cc::async_settled(stranded);
    CHECK(stranded->has_error()).context("a wait the loss cut short fails rather than reporting the work done");
    co_await ctx->epochs_in_flight_completion(0);
    co_await ctx->idle_completion();

    CHECK(ctx->is_device_lost());
    auto const errors = ctx->take_pending_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].kind == sg::device_error_kind::device_lost);
    CHECK_THROWS_AS((void)ctx->create_command_list(), sg::device_lost_exception);

    // The loss was only simulated, so the queue still holds the wait on `blocker` and the signals behind it.
    // Let it run them before the events and the context are released.
    blocker->setSignaledValue(1);
    while (!drained->waitUntilSignaledValue(1, 1000))
    {
    }
    drained->release();
    blocker->release();
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
