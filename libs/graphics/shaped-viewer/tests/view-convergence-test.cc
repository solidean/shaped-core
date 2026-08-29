#include <clean-core/container/array.hh>
#include <clean-core/container/pair.hh>
#include <clean-core/error/optional.hh>
#include <nexus/test.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/view/render_settings.hh>
#include <shaped-viewer/view/view_data.hh>

using namespace cc::primitive_defines;

// How far a view has got, folded over every traced layer it has.
//
// No device: this is arithmetic over the temporal map, and it is the rule two very different callers share — the
// public `view_ref::accumulated_frames` a caller polls, and the capture run deciding its image is finished.
// They disagreed once, which is what these pin.

namespace
{
/// A view carrying one accumulation slot per (layer, frame count) pair, and nothing else.
[[nodiscard]] sv::impl::view_state view_with(cc::span<cc::pair<u8, u32> const> layers)
{
    auto state = sv::impl::view_state();
    for (auto const& [layer, frames] : layers)
        state.temporal[sv::temporal_id::accumulation(layer)].accum_frame = frames;
    return state;
}
} // namespace

// The regression: reading layer 0 by hand reported 0 forever for a view whose trace is not the first layer.
// A layout or a ui layer below the scene is enough to produce that, and it is the shape a real caller writes.
TEST("sv - a view's accumulation is found whatever layer the trace sits on")
{
    auto const layers = cc::array<cc::pair<u8, u32>>{{2, 37}};
    auto const state = view_with(layers);

    CHECK(sv::impl::min_accumulated_frames(state) == 37);

    // What the old code did, kept here as the thing that must NOT be the answer.
    CHECK(state.temporal.get_ptr(sv::temporal_id::accumulation(0)) == nullptr);
}

TEST("sv - a view converges no faster than its slowest traced layer")
{
    auto const layers = cc::array<cc::pair<u8, u32>>{{0, 400}, {1, 12}, {3, 96}};
    auto const state = view_with(layers);

    CHECK(sv::impl::min_accumulated_frames(state) == 12);
    CHECK(!sv::impl::is_accumulation_converged(state, 96));
    CHECK(sv::impl::is_accumulation_converged(state, 12));
}

TEST("sv - only accumulation slots count")
{
    auto state = view_with(cc::array<cc::pair<u8, u32>>{{1, 50}});

    // A reserved id of some other kind is not an accumulator, so it says nothing about convergence.
    state.temporal[(u64(2) << sv::temporal_id::kind_shift) | 1].accum_frame = 0;
    // Neither does a slot a caller declared for itself, which keeps out of the reserved range entirely.
    state.temporal[7].accum_frame = 0;

    CHECK(sv::impl::min_accumulated_frames(state) == 50);
    CHECK(sv::impl::is_accumulation_converged(state, 50));
}

// The case the capture run burned its whole timeout on.
// A layer stops climbing at the cap, so a target above it is one the counter can never reach — and waiting on the
// counter waits forever on an image that finished long ago.
TEST("sv - a layer that has stopped climbing counts as converged")
{
    auto const capped = view_with(cc::array<cc::pair<u8, u32>>{{0, sv::accumulation_frame_cap}});

    CHECK(sv::impl::is_accumulation_converged(capped, sv::accumulation_frame_cap * 2));
    CHECK(sv::impl::is_accumulation_converged(capped, 10));

    // An unset target asks only that question: has this view finished as far as it can.
    CHECK(sv::impl::is_accumulation_converged(capped, {}));
    CHECK(!sv::impl::is_accumulation_converged(view_with(cc::array<cc::pair<u8, u32>>{{0, 60}}), {}));
}

TEST("sv - a view with no traced layer has not converged")
{
    auto const empty = sv::impl::view_state();

    CHECK(sv::impl::min_accumulated_frames(empty) == 0);

    // False rather than true, and deliberately: nothing here has converged, so a caller cannot read "no trace yet"
    // as "finished". A frame with no traced view at all is the capture path's own question, and it answers it before
    // asking this one.
    CHECK(!sv::impl::is_accumulation_converged(empty, 1));
    CHECK(!sv::impl::is_accumulation_converged(empty, {}));
}
