#include <clean-core/container/array.hh>
#include <clean-core/container/pair.hh>
#include <clean-core/error/optional.hh>
#include <nexus/test.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-viewer/impl/capture_session.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/view/render_settings.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/view_store.hh>

using namespace cc::primitive_defines;

// How far a view has got, folded over every traced layer it has, in samples per pixel.
//
// No device: this is arithmetic over the temporal map, and it is the rule two very different callers share — the
// public `view_ref::accumulated_samples` a caller polls, and the capture run deciding its image is finished.
// They disagreed once, which is what these pin.

namespace
{
/// A view carrying one accumulation slot per (layer, samples) pair, each a frame into its mean, and nothing else.
[[nodiscard]] sv::impl::view_state view_with(cc::span<cc::pair<u8, u32> const> layers)
{
    auto state = sv::impl::view_state();
    for (auto const& [layer, samples] : layers)
    {
        auto& slot = state.temporal[sv::temporal_id::accumulation(layer)];
        slot.accum_frame = 1;
        slot.samples = samples;
    }
    return state;
}
} // namespace

// The regression: reading layer 0 by hand reported 0 forever for a view whose trace is not the first layer.
// A layout or a ui layer below the scene is enough to produce that, and it is the shape a real caller writes.
TEST("sv - a view's accumulation is found whatever layer the trace sits on")
{
    auto const layers = cc::array<cc::pair<u8, u32>>{{2, 37}};
    auto const state = view_with(layers);

    CHECK(sv::impl::min_accumulated_samples(state) == 37);

    // What the old code did, kept here as the thing that must NOT be the answer.
    CHECK(state.temporal.get_ptr(sv::temporal_id::accumulation(0)) == nullptr);
}

TEST("sv - a view converges no faster than its slowest traced layer")
{
    auto const layers = cc::array<cc::pair<u8, u32>>{{0, 400}, {1, 12}, {3, 96}};
    auto const state = view_with(layers);

    CHECK(sv::impl::min_accumulated_samples(state) == 12);
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

    CHECK(sv::impl::min_accumulated_samples(state) == 50);
    CHECK(sv::impl::is_accumulation_converged(state, 50));
}

// The case the capture run burned its whole timeout on.
// A layer stops climbing at the cap, so a target above it is one the counter can never reach — and waiting on the
// counter waits forever on an image that finished long ago.
TEST("sv - a layer that has stopped climbing counts as converged")
{
    auto const capped = view_with(cc::array<cc::pair<u8, u32>>{{0, sv::accumulation_sample_cap}});

    CHECK(sv::impl::is_accumulation_converged(capped, sv::accumulation_sample_cap * 2));
    CHECK(sv::impl::is_accumulation_converged(capped, 10));

    // An unset target asks only that question: has this view finished as far as it can.
    CHECK(sv::impl::is_accumulation_converged(capped, {}));
    CHECK(!sv::impl::is_accumulation_converged(view_with(cc::array<cc::pair<u8, u32>>{{0, 60}}), {}));
}

// A layer whose definition asks for fewer samples than the cap stops there, and waiting on it with no count is waiting for that.
TEST("sv - a layer at its definition's sample target has converged")
{
    auto state = view_with(cc::array<cc::pair<u8, u32>>{{0, 256}});
    state.temporal[sv::temporal_id::accumulation(0)].sample_target = 256;
    CHECK(sv::impl::is_accumulation_converged(state, {}));
    CHECK(sv::impl::is_accumulation_converged(state, 1000));

    state.temporal[sv::temporal_id::accumulation(0)].samples = 255;
    CHECK(!sv::impl::is_accumulation_converged(state, {}));
}

// A frame target is counted as frames are recorded, so a wavefront layer reaches it with its sample floor still in flight.
// That is what lets a capture counting iterations settle without waiting on the readback.
TEST("sv - a layer at its frame target has converged whatever its samples say")
{
    auto state = view_with(cc::array<cc::pair<u8, u32>>{{0, 0}});
    auto& slot = state.temporal[sv::temporal_id::accumulation(0)];
    slot.frame_target = 3;
    slot.accum_frame = 2;
    CHECK(!sv::impl::is_accumulation_converged(state, {}));

    slot.accum_frame = 3;
    CHECK(slot.is_at_target());
    CHECK(sv::impl::is_accumulation_converged(state, {}));

    // A restart counts neither frames nor samples, so it is short of any target.
    sv::impl::restart_accumulation(slot);
    CHECK(!slot.is_at_target());

    // No frame target leaves the sample rule alone.
    slot.frame_target = 0;
    slot.accum_frame = 100;
    CHECK(!sv::impl::is_accumulation_converged(state, {}));
}

// A raster layer's frame is its final image, so the renderer marks it at the cap the frame one lands.
// That is what lets a raster view finish a capture whatever it asks for.
TEST("sv - a raster layer at the cap has converged for any target")
{
    auto state = sv::impl::view_state();
    auto& slot = state.temporal[sv::temporal_id::accumulation(0)];
    slot.accum_frame = 1;
    slot.samples = sv::accumulation_sample_cap;

    CHECK(sv::impl::is_accumulation_converged(state, 1));
    CHECK(sv::impl::is_accumulation_converged(state, 1u << 30));
    CHECK(sv::impl::min_accumulated_samples(state) == sv::accumulation_sample_cap);
}

// A wavefront layer's samples read back a frame or more after the frame that restarted its mean.
// Until the restart's own floor lands, what the slot still counts is the old image's, which must not read as this one's.
TEST("sv - a mean that restarted holds no samples, whatever was counted before")
{
    auto state = view_with(cc::array<cc::pair<u8, u32>>{{0, 500}});
    auto& slot = state.temporal[sv::temporal_id::accumulation(0)];

    slot.accum_frame = 0;
    CHECK(sv::impl::min_accumulated_samples(state) == 0);
    CHECK(!sv::impl::is_accumulation_converged(state, 1));

    slot.accum_frame = 3;
    slot.pixels_short = 12;
    // A floor still in flight: the old image's, which must not land on the restarted one.
    slot.sample_floors.push_back(sg::data_future<u32>());
    sv::impl::restart_accumulation(slot);
    CHECK(slot.accum_frame == 0);
    CHECK(slot.samples == 0);
    CHECK(slot.pixels_short == 0);
    CHECK(slot.sample_floors.empty());
}

// The reset a person asks for: the image converging again from nothing, the denoiser's history with it.
TEST("sv - a view's restart starts every traced layer over and cuts its history")
{
    auto store = sv::view_store();
    auto const id = sv::view_id::from_string("restarted");
    auto& state = store.get_or_create(id);
    state.temporal[sv::temporal_id::accumulation(0)].accum_frame = 4;
    state.temporal[sv::temporal_id::accumulation(0)].samples = 64;
    state.temporal[sv::temporal_id::accumulation(3)].accum_frame = 2;
    state.temporal[sv::temporal_id::accumulation(3)].samples = 9;
    state.temporal[sv::temporal_id::normal_guide(0)].accum_frame = 4;

    store.restart_accumulation(id);

    CHECK(store.min_accumulated_samples(id) == 0);
    CHECK(store.accumulated_frames(id) == 0);
    CHECK(store.accumulated_frames(id, sv::temporal_id::accumulation(3)) == 0);
    // The guides restart with the mean on the next trace, and the cut throws the denoiser's history away there.
    CHECK(state.temporal[sv::temporal_id::normal_guide(0)].accum_frame == 4);
    CHECK(state.camera_cut_pending);

    // A view that is not here is no error.
    store.restart_accumulation(sv::view_id::from_string("absent"));
    CHECK(store.peek_ptr(sv::view_id::from_string("absent")) == nullptr);
}

namespace
{
/// The clock a budget test advances by hand: `capture_request::clock_seconds` is a plain function pointer.
double budget_clock = 0.0;
double read_budget_clock()
{
    return budget_clock;
}
} // namespace

// A final render under a time budget: each frame gets what is left over the frames still to come, so a quick frame leaves
// its time to the later ones, and a frame's share running out is the plan rather than a timeout.
TEST("sv - a budgeted sequence gives each frame its share of what is left")
{
    budget_clock = 100.0;
    auto session = sv::impl::capture_session(sr::capture_request{.timeout_seconds = 60.0,
                                                                 .budget_seconds = 40.0,
                                                                 .frames = 4,
                                                                 .clock_seconds = read_budget_clock});
    REQUIRE(session.has_budget());
    session.begin();
    CHECK(session.frame_share_seconds() == 10.0);
    CHECK(!session.is_share_spent());

    // the first frame finishes after 4 s, so the three left share the 36 s still unspent
    budget_clock = 104.0;
    session.advance_frame();
    CHECK(session.frame_share_seconds() == 12.0);

    // the second spends its whole share and more: spent, but nowhere near the timeout
    budget_clock = 117.0;
    CHECK(session.is_share_spent());
    CHECK(!session.is_out_of_time());
    session.advance_frame();
    CHECK(session.frame_share_seconds() == 11.5);
    CHECK(session.run_elapsed_seconds() == 17.0);

    // a run past its budget gives the frames left nothing, and each then ends on its first finished frame
    budget_clock = 150.0;
    session.advance_frame();
    CHECK(session.frame_share_seconds() == 0.0);
    CHECK(session.is_share_spent());

    // a share longer than the timeout moves the timeout past it, so spending the share is never a failure
    budget_clock = 0.0;
    auto long_still = sv::impl::capture_session(
        sr::capture_request{.timeout_seconds = 60.0, .budget_seconds = 300.0, .clock_seconds = read_budget_clock});
    long_still.begin();
    budget_clock = 200.0;
    CHECK(!long_still.is_share_spent());
    CHECK(!long_still.is_out_of_time());
    budget_clock = 305.0;
    CHECK(long_still.is_share_spent());
    CHECK(!long_still.is_out_of_time());
    budget_clock = 311.0;
    CHECK(long_still.is_out_of_time());

    // without a budget nothing is ever a share
    auto plain = sv::impl::capture_session(sr::capture_request{.frames = 4, .clock_seconds = read_budget_clock});
    plain.begin();
    CHECK(!plain.has_budget());
    CHECK(!plain.is_share_spent());
}

TEST("sv - a capture restarts its views each time owed work drains, and only then")
{
    auto session = sv::impl::capture_session(sr::capture_request{});

    // Nothing owed from the start: the views were traced against the final resources all along.
    CHECK(!session.note_owed_work(0));
    CHECK(!session.note_owed_work(0));

    // Restarted once, on the first frame owing nothing, and not again while nothing is owed.
    CHECK(!session.note_owed_work(3));
    CHECK(!session.note_owed_work(1));
    CHECK(session.note_owed_work(0));
    CHECK(!session.note_owed_work(0));

    // Work that comes back, a texture landing late, restarts them again once it drains.
    CHECK(!session.note_owed_work(1));
    CHECK(session.note_owed_work(0));
}

// What a capture does when owed work drains: a BC7 encode replaces a texture's contents under the same id, so no trace restarts on it.
// A view that already stopped at its target would keep samples of the uncompressed texture, all of them on a fast GPU and some on a slow one.
// A video streaming geometry into every frame drains on every frame, and cutting the history there left DLSS starting cold on each.
// The first frame's drain lands an asset's loads and encodes under the same texture ids; a temporal member carried across it
// hung the GPU on a later viewer, so that drain cuts the history, and only that one: a video streaming content keeps it.
TEST("sv - a capture's drain cuts the temporal history on the run's first frame alone")
{
    auto session = sv::impl::capture_session(sr::capture_request{.frames = 3});
    CHECK(session.drain_cuts_history());
    session.advance_frame();
    CHECK(!session.drain_cuts_history());
    session.advance_frame();
    CHECK(!session.drain_cuts_history());
    CHECK(sv::impl::capture_session(sr::capture_request{}).drain_cuts_history());
}

TEST("sv - restarting a view's means keeps its temporal history")
{
    auto store = sv::view_store();
    auto const id = sv::view_id::from_string("streaming");
    auto& state = store.get_or_create(id);
    state.temporal[sv::temporal_id::accumulation(0)].accum_frame = 4;
    state.temporal[sv::temporal_id::accumulation(0)].samples = 64;

    store.restart_means(id);

    CHECK(store.min_accumulated_samples(id) == 0);
    CHECK(store.accumulated_frames(id) == 0);
    CHECK(!state.camera_cut_pending);

    store.restart_means(sv::view_id::from_string("absent"));
    CHECK(store.peek_ptr(sv::view_id::from_string("absent")) == nullptr);
}

// What a capture that gives up names: every traced layer's pixels below its target, but none of a mean that restarted.
TEST("sv - the pixels short of a target are summed over the traced layers that hold a mean")
{
    auto state = view_with(cc::array<cc::pair<u8, u32>>{{0, 3}, {1, 5}, {2, 7}});
    state.temporal[sv::temporal_id::accumulation(0)].pixels_short = 4;
    state.temporal[sv::temporal_id::accumulation(1)].pixels_short = 9;
    state.temporal[sv::temporal_id::accumulation(2)].pixels_short = 100;
    state.temporal[sv::temporal_id::accumulation(2)].accum_frame = 0;

    CHECK(sv::impl::pixels_short_of_target(state) == 13);
    CHECK(sv::impl::pixels_short_of_target(sv::impl::view_state()) == 0);
}

TEST("sv - a view with no traced layer has not converged")
{
    auto const empty = sv::impl::view_state();

    CHECK(sv::impl::min_accumulated_samples(empty) == 0);

    // False rather than true, and deliberately: nothing here has converged, so a caller cannot read "no trace yet"
    // as "finished". A frame with no traced view at all is the capture path's own question, and it answers it before
    // asking this one.
    CHECK(!sv::impl::is_accumulation_converged(empty, 1));
    CHECK(!sv::impl::is_accumulation_converged(empty, {}));
}
