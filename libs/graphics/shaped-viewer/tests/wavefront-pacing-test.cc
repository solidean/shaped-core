#include <nexus/test.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <shaped-viewer/viewer.hh>

using namespace cc::primitive_defines;

// How a still wavefront frame is paced, as rules on numbers: what a GPU measured goes in, rounds come out.
// The viewer-level half, that a still layer's rounds follow its own timestamps, is view-accumulation-test.cc's.

TEST("sv::impl - a still wavefront frame runs its rounds until a round is measured")
{
    CHECK(sv::impl::still_rounds_for(8, 16.0f, 0.0) == 8);
    CHECK(sv::impl::still_rounds_for(8, 0.0f, 1e-3) == 8);
    // Fewer than one round is still one.
    CHECK(sv::impl::still_rounds_for(0, 0.0f, 0.0) == 1);
}

TEST("sv::impl - a measured round fits the budget, between one round and the most a budget gives")
{
    // 16 ms at 1 ms a round.
    CHECK(sv::impl::still_rounds_for(8, 16.0f, 1e-3) == 16);
    // A round dearer than the whole budget still runs once.
    CHECK(sv::impl::still_rounds_for(8, 16.0f, 0.1) == 1);
    // A round almost free is capped.
    CHECK(sv::impl::still_rounds_for(8, 16.0f, 1e-9) == sv::impl::max_budgeted_rounds);
}

TEST("sv::impl - the frame's budget is split among its still wavefront layers")
{
    CHECK(sv::impl::wavefront_budget_share(16.0f, 4) == 4.0f);
    CHECK(sv::impl::wavefront_budget_share(16.0f, 1) == 16.0f);
    // No still layer yet takes the whole, which the first still frame is.
    CHECK(sv::impl::wavefront_budget_share(16.0f, 0) == 16.0f);
    CHECK(sv::impl::wavefront_budget_share(0.0f, 3) == 0.0f);
}

TEST("sv::impl - a headless viewer gives its wavefront layers no budget")
{
    CHECK(sv::impl::wavefront_budget_of({.frame_budget_ms = 16.0f}) == 16.0f);
    CHECK(sv::impl::wavefront_budget_of({.headless = true, .frame_budget_ms = 16.0f}) == 0.0f);
    CHECK(sv::impl::wavefront_budget_of({.frame_budget_ms = -1.0f}) == 0.0f);
}

TEST("sv::wavefront_routine - a draining frame runs every bounce and a medium walk up to its roulette")
{
    CHECK(sv::wavefront_routine::drain_rounds(5) == 5 + 1 + 16);
    CHECK(sv::wavefront_routine::drain_rounds(0) == 1 + 16);
    CHECK(sv::wavefront_routine::drain_rounds(-3) == 1 + 16);
}
