#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>

using namespace cc::primitive_defines;
using sg::impl::transfer_candidate;
using sg::impl::transfer_flavor;
using sg::impl::transfer_scheduler;

// The transfer job-selection policy, with no GPU and no backend in sight.
// Nearly all of this system's risk is policy rather than D3D12, which is why it is factored to be testable here:
// window sharing, priority, aging, family ordering and eligibility are all decided by these two entry points.

namespace
{
[[nodiscard]] transfer_candidate async_job(u64 sequence, u64 family, bool eligible = true)
{
    return {.flavor = transfer_flavor::async, .family = family, .sequence = sequence, .eligible = eligible};
}

[[nodiscard]] transfer_candidate stream_job(u64 sequence, u64 family, i32 priority, bool eligible = true)
{
    return {.flavor = transfer_flavor::streaming,
            .priority = priority,
            .family = family,
            .sequence = sequence,
            .eligible = eligible};
}
} // namespace

TEST("sg transfer_scheduler - nothing to pick")
{
    transfer_scheduler s;
    s.begin_window();
    CHECK(!s.pick_next({}).has_value());

    auto const blocked = cc::vector<transfer_candidate>{async_job(0, 1, false)};
    CHECK(!s.pick_next(blocked).has_value());
}

TEST("sg transfer_scheduler - async runs first-in-first-out")
{
    transfer_scheduler s;
    s.begin_window();

    // Deliberately out of order in the span: selection must key off sequence, not position.
    auto const jobs = cc::vector<transfer_candidate>{async_job(7, 70), async_job(2, 20), async_job(5, 50)};
    auto const pick = s.pick_next(jobs);
    REQUIRE(pick.has_value());
    CHECK(jobs[pick.value()].sequence == 2);
}

TEST("sg transfer_scheduler - a family runs in sequence order")
{
    transfer_scheduler s;
    s.begin_window();

    // Both target the same resource, so the later one must not overtake the earlier.
    auto const jobs = cc::vector<transfer_candidate>{async_job(9, 1), async_job(4, 1)};
    auto const pick = s.pick_next(jobs);
    REQUIRE(pick.has_value());
    CHECK(jobs[pick.value()].sequence == 4);
}

TEST("sg transfer_scheduler - an ineligible head blocks only its own family")
{
    transfer_scheduler s;
    s.begin_window();

    // Family 1's head is waiting on something; family 2 must be filled around it rather than queueing behind it.
    // This is the whole point of out-of-order selection — head-of-line blocking is what it exists to remove.
    auto const jobs = cc::vector<transfer_candidate>{async_job(0, 1, false), async_job(1, 1), async_job(2, 2)};
    auto const pick = s.pick_next(jobs);
    REQUIRE(pick.has_value());
    CHECK(jobs[pick.value()].sequence == 2); // NOT 1: family 1 still waits on its own head
}

TEST("sg transfer_scheduler - streaming picks highest priority, then oldest")
{
    transfer_scheduler s;
    s.set_stream_ratio(1.0f);
    s.on_window_submitted(0, 0); // nothing moved, so streaming is owed nothing yet
    s.begin_window();
    CHECK(s.window_primary() == transfer_flavor::async);

    auto const jobs = cc::vector<transfer_candidate>{stream_job(1, 10, 5), stream_job(2, 20, 9), stream_job(3, 30, 9),
                                                     stream_job(4, 40, 1)};
    auto const pick = s.pick_next(jobs); // no async work, so streaming fills the leftover
    REQUIRE(pick.has_value());
    CHECK(jobs[pick.value()].sequence == 2); // priority 9, and the older of the two nines
}

TEST("sg transfer_scheduler - aging is off unless asked for")
{
    auto const jobs = cc::vector<transfer_candidate>{
        transfer_candidate{.flavor = transfer_flavor::streaming, .priority = 10, .age_seconds = 0, .family = 1, .sequence = 1},
        transfer_candidate{.flavor = transfer_flavor::streaming,
                           .priority = 0,
                           .age_seconds = 100,
                           .family = 2,
                           .sequence = 2},
    };

    transfer_scheduler off;
    off.begin_window();
    auto const a = off.pick_next(jobs);
    REQUIRE(a.has_value());
    CHECK(jobs[a.value()].priority == 10); // a low-priority job never overtakes, however long it has waited

    transfer_scheduler aging;
    aging.set_aging_factor(1.0f); // 100 s of waiting is worth 100 priority
    aging.begin_window();
    auto const b = aging.pick_next(jobs);
    REQUIRE(b.has_value());
    CHECK(jobs[b.value()].priority == 0);
}

TEST("sg transfer_scheduler - the primary flavor is served first, the other fills the leftover")
{
    transfer_scheduler s;
    auto const jobs = cc::vector<transfer_candidate>{async_job(1, 10), stream_job(2, 20, 0)};

    s.begin_window(); // no deficit yet → async is primary
    CHECK(s.window_primary() == transfer_flavor::async);
    auto const first = s.pick_next(jobs);
    REQUIRE(first.has_value());
    CHECK(jobs[first.value()].flavor == transfer_flavor::async);

    // Move a window's worth of async bytes; streaming is now owed its share.
    s.set_window_bytes(1000);
    s.on_window_submitted(1000, 0);
    CHECK(s.stream_deficit_bytes() == 100); // 10 % of what moved

    s.begin_window();
    CHECK(s.window_primary() == transfer_flavor::streaming);
    auto const second = s.pick_next(jobs);
    REQUIRE(second.has_value());
    CHECK(jobs[second.value()].flavor == transfer_flavor::streaming);
}

TEST("sg transfer_scheduler - a stream-primary window falls back to async when streaming has nothing")
{
    transfer_scheduler s;
    s.set_window_bytes(1000);
    s.on_window_submitted(1000, 0);
    s.begin_window();
    REQUIRE(s.window_primary() == transfer_flavor::streaming);

    auto const only_async = cc::vector<transfer_candidate>{async_job(1, 10)};
    auto const pick = s.pick_next(only_async);
    REQUIRE(pick.has_value()); // the window is not left empty just because its primary had no work
    CHECK(only_async[pick.value()].flavor == transfer_flavor::async);
}

TEST("sg transfer_scheduler - the deficit tracks bytes actually moved")
{
    transfer_scheduler s;
    s.set_window_bytes(10000);
    s.set_stream_ratio(0.1f);

    // A stream-primary window that only manages a tenth of a window must not burn the whole share.
    s.on_window_submitted(0, 100);
    CHECK(s.stream_deficit_bytes() == 10 - 100); // owed 10 of the 100 that moved, and it took all 100

    s.on_window_submitted(1000, 0);
    CHECK(s.stream_deficit_bytes() == -90 + 100);
}

TEST("sg transfer_scheduler - idle streaming cannot bank unlimited credit")
{
    transfer_scheduler s;
    s.set_window_bytes(500);
    s.set_stream_ratio(0.5f);

    for (int i = 0; i < 100; ++i)
        s.on_window_submitted(1000, 0); // async hammers away, streaming never shows up

    // One window's worth is the bound: enough to guarantee the next window, never enough to seize ten of them.
    CHECK(s.stream_deficit_bytes() == 500);
}

TEST("sg transfer_scheduler - a zero ratio lets async starve streaming")
{
    transfer_scheduler s;
    s.set_window_bytes(1000);
    s.set_stream_ratio(0.0f);

    s.on_window_submitted(1000, 0);
    CHECK(s.stream_deficit_bytes() == 0);
    s.begin_window();
    CHECK(s.window_primary() == transfer_flavor::async);

    // Streaming still runs when async has nothing — the ratio is a floor on share, not a gate on progress.
    auto const only_stream = cc::vector<transfer_candidate>{stream_job(1, 10, 0)};
    CHECK(s.pick_next(only_stream).has_value());
}

TEST("sg transfer_scheduler - aging needs a real age to act on")
{
    // The regression this pins is not the arithmetic — that is covered above — but the wiring.
    // An actor that never fills age_seconds leaves every candidate at 0, and a caller who turned aging on gets a
    // knob that is accepted, documented and inert.
    transfer_scheduler aging;
    aging.set_aging_factor(1.0f);
    aging.begin_window();

    auto const unaged = cc::vector<transfer_candidate>{
        transfer_candidate{.flavor = transfer_flavor::streaming, .priority = 10, .family = 1, .sequence = 1},
        transfer_candidate{.flavor = transfer_flavor::streaming, .priority = 0, .family = 2, .sequence = 2},
    };
    auto const without_age = aging.pick_next(unaged);
    REQUIRE(without_age.has_value());
    CHECK(unaged[without_age.value()].priority == 10); // no age means aging cannot change anything

    auto aged = unaged;
    aged[1].age_seconds = 100; // the same jobs, one of them having actually waited
    auto const with_age = aging.pick_next(aged);
    REQUIRE(with_age.has_value());
    CHECK(aged[with_age.value()].priority == 0);
}
