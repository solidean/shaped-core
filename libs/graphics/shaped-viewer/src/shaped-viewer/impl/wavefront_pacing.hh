#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh> // wavefront_timing, held by value

namespace sv::impl
{
/// What one still wavefront frame runs: its rounds, and the share of free slots that start a new sample.
struct wavefront_plan
{
    i32 rounds = 1;
    f32 fill = 1.0f;
};

/// How a still wavefront frame fits its time budget, from what its earlier frames measured.
///
/// Rounds come first: a frame runs as many full rounds as the budget holds.
/// Only where one round at full occupancy overruns it does `fill` fall below 1, and then the frame runs one round over fewer paths.
/// A round's cost grows slower than `fill`, since the paths it carries stay, so each measured frame moves `fill` toward the budget rather than solving for it.
struct wavefront_pacing
{
    /// The least share of free slots a frame starts, so a very slow round still lets every pixel progress.
    static constexpr f32 min_fill = 1.0f / 64.0f;

    /// A fill this close to 1, moving toward a target as close, is 1: a full round that overruns the budget by less than 5% keeps whole rounds.
    static constexpr f32 full_fill = 0.95f;

    /// The most rounds a budget gives a still frame, however cheap a round measured.
    static constexpr i32 max_rounds = 64;

    /// The still frames after a restart whose one-round times steer nothing.
    /// The restart drained every slot, so such a frame holds fewer paths than its fill keeps once the carried paths are back.
    static constexpr i32 warmup_frames = 8;

    /// GPU seconds a round at full fill costs, or 0 before any frame landed.
    /// A frame at fill `f` contributes its round's cost over `f`, which overestimates it, since the cost grows slower than the fill.
    f64 seconds_per_full_round = 0.0;

    /// The share of free slots the next frame starts.
    f32 fill = 1.0f;

    /// Still frames queued since the last restart.
    i32 still_frames = 0;

    /// One traced frame, kept until its timestamps land.
    struct pending
    {
        wavefront_timing timing = {};
        i32 rounds = 0;
        f32 fill = 1.0f;
        bool warming = false;
    };
    cc::vector<pending> in_flight;

    /// Notes a frame that restarted, and so drained every slot.
    void note_restart();

    /// Queues the still frame about to run `plan`, and returns where its timestamps go.
    [[nodiscard]] wavefront_timing& queue(wavefront_plan plan);

    /// Folds in the landed frame `p`, which took `seconds` of GPU time against `budget_seconds`.
    void land(pending const& p, f64 seconds, f64 budget_seconds);

    /// What the next still frame runs under `budget_seconds`, or `fallback_rounds` at full fill before anything landed.
    [[nodiscard]] wavefront_plan next(f64 budget_seconds, i32 fallback_rounds) const;

    /// Forgets every measurement, for a frame with no budget.
    void reset();
};
} // namespace sv::impl
