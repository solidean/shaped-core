#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/impl/wavefront_pacing.hh>
#include <shaped-viewer/rendering/render_plan.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/view_store.hh>
#include <shaped-viewer/view/viewer_definition.hh>
#include <shaped-viewer/viewer.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

using namespace cc::primitive_defines;

// How a still wavefront frame fits its budget, against a modeled GPU.
//
// No device: the controller is arithmetic over landed frame times, and the model is what makes its corrections land
// the way a GPU's would — late, and with a round's cost growing slower than its fill.

namespace
{
constexpr f64 k_budget = 0.016;

/// A still round's GPU seconds at `fill`, for a scene whose full round costs `full_round` and whose paths live `m` rounds.
/// Paths that carry over stay, so live slots are `fill * m / (1 + fill * m)` of a full round's, not `fill` of them.
[[nodiscard]] f64 modeled_round(f64 full_round, f64 fill, f64 m)
{
    auto const live = fill * m / (1.0 + fill * m);
    auto const full = m / (1.0 + m);
    return full_round * live / full;
}

/// The last frame a run planned, and what it cost.
struct pacing_run
{
    sv::impl::wavefront_plan last = {};
    f64 last_seconds = 0.0;
};

/// Runs `frames` still frames through `pacing`, each landing `latency` frames after it ran, as timestamps do, and the rest at the end.
/// A frame's cost is the model's, scaled by `transient(f)`, which lets a run stand in for the frames after a restart.
template <class Transient>
[[nodiscard]] pacing_run run_frames(sv::impl::wavefront_pacing& pacing, f64 full_round, int frames, Transient transient)
{
    constexpr isize latency = 2;
    constexpr f64 path_rounds = 3.5;
    auto costs = cc::vector<f64>();
    auto out = pacing_run{};
    for (auto f = 0; f < frames; ++f)
    {
        if (costs.size() >= latency)
        {
            pacing.land(pacing.in_flight.front(), costs.front(), k_budget);
            pacing.in_flight.remove_at(0);
            costs.remove_at(0);
        }
        auto const plan = pacing.next(k_budget, 8);
        (void)pacing.queue(plan);
        auto const seconds = transient(f) * f64(plan.rounds) * modeled_round(full_round, f64(plan.fill), path_rounds);
        costs.push_back(seconds);
        out = {.last = plan, .last_seconds = seconds};
    }
    // Everything still in flight lands, so the next run's frames pair with their own costs.
    for (auto const seconds : costs)
    {
        pacing.land(pacing.in_flight.front(), seconds, k_budget);
        pacing.in_flight.remove_at(0);
    }
    return out;
}

[[nodiscard]] pacing_run run_frames(sv::impl::wavefront_pacing& pacing, f64 full_round, int frames)
{
    return run_frames(pacing, full_round, frames, [](int) { return 1.0; });
}
} // namespace

TEST("sv::impl::wavefront_pacing - a round that overruns the budget lowers the fill until one fits")
{
    auto pacing = sv::impl::wavefront_pacing();
    auto const run = run_frames(pacing, 0.028, 60);

    CHECK(run.last.rounds == 1);
    CHECK(run.last.fill < 1.0f);
    CHECK(run.last.fill > sv::impl::wavefront_pacing::min_fill);
    CHECK(tg::abs(run.last_seconds - k_budget) < 0.1 * k_budget)
        .context(cc::format("a frame of fill {} costs {} s against {}", run.last.fill, run.last_seconds, k_budget));
}

TEST("sv::impl::wavefront_pacing - a round that fits runs whole rounds at full fill")
{
    auto pacing = sv::impl::wavefront_pacing();
    // 0.016 / 0.0015 holds 10 rounds, so the plan is the measurement's and not the fallback's 8.
    auto const run = run_frames(pacing, 0.0015, 30);

    CHECK(run.last.fill == 1.0f);
    CHECK(run.last.rounds == 10);
}

// A view that turns from a dense scene to an empty sky must regain its rounds, not stay one round at the fill it last needed.
TEST("sv::impl::wavefront_pacing - a scene that becomes cheap regains full fill and whole rounds")
{
    auto pacing = sv::impl::wavefront_pacing();
    (void)run_frames(pacing, 0.028, 60);
    REQUIRE(pacing.fill < 1.0f);

    auto const run = run_frames(pacing, 0.002, 60);
    CHECK(run.last.fill == 1.0f);
    CHECK(run.last.rounds > 1);
    CHECK(run.last_seconds <= 1.1 * k_budget);
}

TEST("sv::impl::wavefront_pacing - a scene that becomes expensive drops from whole rounds to a fill that fits")
{
    auto pacing = sv::impl::wavefront_pacing();
    (void)run_frames(pacing, 0.002, 60);
    REQUIRE(pacing.fill == 1.0f);

    auto const run = run_frames(pacing, 0.028, 60);
    CHECK(run.last.rounds == 1);
    CHECK(run.last.fill < 1.0f);
    CHECK(tg::abs(run.last_seconds - k_budget) < 0.1 * k_budget);
}

// The restart drained every slot, so the first still frames after it hold only what their fill started and cost far less.
// Steering by them would raise the fill, and the frames after the warm-up would overrun until the correction landed.
TEST("sv::impl::wavefront_pacing - the cheap frames right after a restart leave the fill alone")
{
    auto pacing = sv::impl::wavefront_pacing();
    (void)run_frames(pacing, 0.028, 60);
    auto const settled = pacing.fill;
    REQUIRE(settled < 1.0f);

    pacing.note_restart();
    constexpr int warmup = sv::impl::wavefront_pacing::warmup_frames;
    (void)run_frames(pacing, 0.028, warmup, [](int) { return 0.3; });
    CHECK(tg::abs(pacing.fill - settled) < 0.01f)
        .context(cc::format("fill {} after the warm-up, {} before", pacing.fill, settled));
}

TEST("sv::impl::wavefront_pacing - a frame with nothing measured runs the fallback at full fill")
{
    auto const pacing = sv::impl::wavefront_pacing();
    auto const plan = pacing.next(k_budget, 6);
    CHECK(plan.rounds == 6);
    CHECK(plan.fill == 1.0f);
}

// A frame that declines never records timestamps, and the queue lands in order, so one left queued would hold every later frame back.
TEST("sv::impl::wavefront_pacing - a withdrawn frame leaves the queue and the warm-up as they were")
{
    auto pacing = sv::impl::wavefront_pacing();
    (void)pacing.queue({.rounds = 1, .fill = 1.0f});
    auto const queued = pacing.in_flight.size();
    auto const still = pacing.still_frames;

    (void)pacing.queue({.rounds = 2, .fill = 1.0f});
    pacing.withdraw();
    CHECK(pacing.in_flight.size() == queued);
    CHECK(pacing.still_frames == still);
    CHECK(pacing.in_flight.back().rounds == 1);
}

// A budget meant as "no limit" fits more rounds than an i32 holds, so the plan is clamped before it is counted.
TEST("sv::impl::wavefront_pacing - a budget far past any round runs the most rounds")
{
    auto pacing = sv::impl::wavefront_pacing();
    pacing.seconds_per_full_round = 1e-9;
    auto const plan = pacing.next(1e6, 8);
    CHECK(plan.rounds == sv::impl::wavefront_pacing::max_rounds);
    CHECK(plan.fill == 1.0f);
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

// Two still wavefront views split the budget, but one at its sample target traces nothing, so the other gets all of it.
TEST("sv::impl - a view at its sample target takes no share of the frame budget")
{
    auto def = sv::viewer_definition{};
    auto plan = sv::render_plan{};
    auto store = sv::view_store{};
    for (auto i = 0; i < 2; ++i)
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string(cc::format("view {}", i));
        sv::ensure_scene_3d(v).settings.mode = sv::render_mode::wavefront;
        plan.traces.push_back({.id = v.id, .view = sv::view_index(i), .layer = 0});
        auto& slot = store.get_or_create(v.id).temporal[sv::temporal_id::accumulation(0)];
        slot.accum_frame = 3;
        slot.samples = 40;
        slot.sample_target = 256;
        def.views.push_back(cc::move(v));
    }
    CHECK(sv::impl::still_wavefront_traces(def, plan, store) == 2);
    CHECK(sv::impl::wavefront_budget_share(16.0f, sv::impl::still_wavefront_traces(def, plan, store)) == 8.0f);

    store.get_or_create(def.views[0].id).temporal[sv::temporal_id::accumulation(0)].samples = 256;
    CHECK(sv::impl::still_wavefront_traces(def, plan, store) == 1);
    CHECK(sv::impl::wavefront_budget_share(16.0f, sv::impl::still_wavefront_traces(def, plan, store)) == 16.0f);
}
