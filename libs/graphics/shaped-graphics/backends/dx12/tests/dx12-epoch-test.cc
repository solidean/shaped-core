#include "dx12-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Epoch system: the frame-level GPU lifetime counter/fence — advance/retire, deferred deletion of resources, the per-submission completion token, and in-flight throttling.
// The "a list must be submitted/dropped in the epoch it was opened in" and "no open lists at advance" contracts are CC_ASSERT-enforced.
// Their abort paths are untested, since nexus has no death test.
// See libs/graphics/shaped-graphics/docs/concepts/epochs.md.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

// The pristine values are the context's own subject, so this one takes a context of its own rather than the shared one.
TEST("sg dx12 - a new context opens epoch::first, with nothing completed")
{
    auto handle = dx12::make_fresh_context();
    if (handle == nullptr)
        SKIP("no dx12 adapter on this machine");

    CHECK(handle->current_epoch() == sg::epoch::first);
    // Nothing has finished yet, so the completed epoch is first - 1, which is what lets a plain `<=` work from the first frame.
    CHECK(u64(handle->completed_epoch()) == u64(sg::epoch::first) - 1);
}

ASYNC_INVOCABLE_TEST("sg dx12 - epoch advance and retire", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const open = c.current_epoch();
    CHECK(u64(open) >= u64(sg::epoch::first));
    // The open epoch has not finished, whatever finished before it.
    CHECK(u64(c.completed_epoch()) < u64(open));

    c.advance_epoch();
    co_await c.idle_completion();
    CHECK(c.current_epoch() == sg::epoch(u64(open) + 1));
    CHECK(u64(c.completed_epoch()) >= u64(open)); // the epoch that was open is now done
}

ASYNC_INVOCABLE_TEST("sg dx12 - deferred deletion runs finalizers only after the owning epoch retires",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    bool finalized = false;
    {
        auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_dst);
        REQUIRE(buf != nullptr);
        buf->add_finalizer([&finalized] { finalized = true; });
    } // last handle dropped here → deferred deletion staged in the current epoch

    // The owning epoch has not advanced/retired yet, so the resource is still (potentially) in use.
    CHECK(!finalized);

    c.advance_epoch();
    co_await c.idle_completion(); // closes + drains the epoch the buffer died in
    CHECK(finalized);
}

ASYNC_INVOCABLE_TEST("sg dx12 - submission token reports completion", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    auto const token = c.submit_command_list(cc::move(cmd));

    c.advance_epoch();
    co_await c.idle_completion(); // forces the GPU to catch up
    CHECK(c.is_submission_complete(token));
    CHECK(!c.is_submission_complete(sg::submission_token::not_submitted));
}

ASYNC_INVOCABLE_TEST("sg dx12 - throttle bounds epochs in flight", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    // Allow at most one prior epoch in flight; after several advances the FIFO stays bounded.
    for (int i = 0; i < 5; ++i)
        c.advance_epoch();
    co_await c.epochs_in_flight_completion(1);

    auto const in_flight = c._epoch_state.lock([](dx12::dx12_epoch_state& s) { return s.in_flight.size(); });
    CHECK(in_flight <= 1);
}
