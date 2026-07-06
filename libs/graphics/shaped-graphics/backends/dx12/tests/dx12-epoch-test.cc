#include "dx12-test-common.hh"

#include <nexus/test.hh>

// Epoch system: the frame-level GPU lifetime counter/fence — advance/retire, deferred deletion of
// resources, the per-submission completion token, and in-flight throttling. All on WARP so they
// exercise a live epoch fence on headless CI. Note: the "a list must be submitted/dropped in the
// epoch it was opened in" and "no open lists at advance" contracts are CC_ASSERT-enforced; we don't
// test the abort paths (nexus has no death test). See
// libs/graphics/shaped-graphics/docs/concepts/epochs.md.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - epoch advance and retire")
{
    auto handle = dx12::make_warp_context(); // fresh: this asserts the epoch counter's initial value
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    CHECK(c.current_epoch() == sg::epoch::first);
    // Nothing has finished yet, so the completed epoch is first-1.
    CHECK(cc::u64(c.completed_epoch()) == cc::u64(sg::epoch::first) - 1);

    c.advance_epoch_and_wait_for_idle();
    CHECK(c.current_epoch() == sg::epoch(cc::u64(sg::epoch::first) + 1));
    CHECK(cc::u64(c.completed_epoch()) >= cc::u64(sg::epoch::first)); // the first epoch is now done
}

TEST("sg dx12 - deferred deletion runs finalizers only after the owning epoch retires")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    bool finalized = false;
    {
        auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
        REQUIRE(buf.has_value());
        buf.value()->add_finalizer([&finalized] { finalized = true; });
    } // last handle dropped here → deferred deletion staged in the current epoch

    // The owning epoch has not advanced/retired yet, so the resource is still (potentially) in use.
    CHECK(!finalized);

    c.advance_epoch_and_wait_for_idle(); // closes + drains the epoch the buffer died in
    CHECK(finalized);
}

TEST("sg dx12 - submission token reports completion")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    auto const token = c.submit_dx12_command_list(cc::move(cmd.value()));

    c.advance_epoch_and_wait_for_idle(); // forces the GPU to catch up
    CHECK(c.is_submission_complete(token));
    CHECK(!c.is_submission_complete(sg::submission_token::not_submitted));
}

TEST("sg dx12 - throttle bounds epochs in flight")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // Allow at most one prior epoch in flight; after several advances the FIFO stays bounded.
    for (int i = 0; i < 5; ++i)
        c.advance_epoch(1);

    auto const in_flight = c._epoch_state.lock([](dx12::dx12_epoch_state& s) { return s.in_flight.size(); });
    CHECK(in_flight <= 1);
}
