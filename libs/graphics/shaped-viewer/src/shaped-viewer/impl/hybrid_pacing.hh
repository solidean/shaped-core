#pragma once

#include <clean-core/common/utility.hh> // cc::clamp, cc::max
#include <clean-core/container/vector.hh>
#include <shaped-graphics/query/gpu_timestamp.hh>
#include <shaped-viewer/fwd.hh>

namespace sv::impl
{
/// How many samples a still hybrid frame packs to fit its share of a time budget, from the GPU time its earlier packs measured.
///
/// A packed sample is exactly the sample a frame of its own would have drawn, so the count changes how fast the mean climbs and never what it converges to.
/// Before anything lands, and on a device that times nothing, a frame packs one.
struct hybrid_pacing
{
    /// The most samples one frame packs, however cheap a sample measured.
    static constexpr i32 max_samples = 32;

    /// GPU seconds one sample costs, smoothed, or 0 before any pack landed.
    f64 seconds_per_sample = 0.0;

    /// One pack's timestamps, kept until they land.
    struct pending
    {
        sg::gpu_timestamp begin;
        sg::gpu_timestamp end;
        i32 samples = 0;
    };
    cc::vector<pending> in_flight;

    /// Folds in every pack whose timestamps have landed, and returns how many samples the next one packs under `budget_seconds`.
    [[nodiscard]] i32 next(f64 budget_seconds)
    {
        while (!in_flight.empty())
        {
            auto const begin = in_flight.front().begin.try_get_seconds();
            auto const end = in_flight.front().end.try_get_seconds();
            if (!begin.has_value() || !end.has_value())
                break;
            auto const per_sample = (end.value() - begin.value()) / f64(cc::max(1, in_flight.front().samples));
            if (per_sample > 0.0)
                seconds_per_sample = seconds_per_sample <= 0.0 ? per_sample : 0.7 * seconds_per_sample + 0.3 * per_sample;
            in_flight.remove_at(0);
        }

        // A device that times nothing never lands one, so the queue is bounded rather than grown forever.
        constexpr isize max_in_flight = 8;
        while (in_flight.size() > max_in_flight)
            in_flight.remove_at(0);

        if (seconds_per_sample <= 0.0)
            return 1;
        return i32(cc::clamp(budget_seconds / seconds_per_sample, 1.0, f64(max_samples)));
    }

    /// Forgets every measurement, for a frame with no budget.
    void reset()
    {
        seconds_per_sample = 0.0;
        in_flight.clear();
    }
};
} // namespace sv::impl
