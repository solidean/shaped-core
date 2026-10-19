#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-viewer/impl/wavefront_pacing.hh>

namespace sv::impl
{
void wavefront_pacing::note_restart()
{
    still_frames = 0;
}

wavefront_timing& wavefront_pacing::queue(wavefront_plan plan)
{
    in_flight.push_back({.rounds = plan.rounds, .fill = plan.fill, .warming = still_frames < warmup_frames});
    ++still_frames;
    return in_flight.back().timing;
}

void wavefront_pacing::withdraw()
{
    CC_ASSERT(!in_flight.empty(), "only a queued frame can be withdrawn");
    in_flight.pop_back();
    --still_frames;
}

void wavefront_pacing::land(pending const& p, f64 seconds, f64 budget_seconds)
{
    auto const per_round = seconds / f64(cc::max(1, p.rounds));
    // A frame of several rounds runs at full fill, which begin restores at once, so only a one-round frame is cheap during a warm-up.
    if (per_round <= 0.0 || (p.warming && p.rounds == 1))
        return;
    auto const full_round = per_round / f64(cc::max(p.fill, min_fill));
    seconds_per_full_round = seconds_per_full_round <= 0.0 ? full_round : 0.7 * seconds_per_full_round + 0.3 * full_round;

    // A frame of several rounds already fit at full fill, so only a one-round frame says what fill the budget holds.
    if (p.rounds != 1)
        return;
    auto const target = cc::clamp(f64(p.fill) * budget_seconds / per_round, f64(min_fill), 1.0);
    auto const blended = f32(0.5 * f64(fill) + 0.5 * target);
    fill = target >= f64(full_fill) && blended >= full_fill ? 1.0f : blended;
}

wavefront_plan wavefront_pacing::next(f64 budget_seconds, i32 fallback_rounds) const
{
    if (fill < 1.0f)
        return {.rounds = 1, .fill = fill};
    if (seconds_per_full_round <= 0.0)
        return {.rounds = cc::max(1, fallback_rounds), .fill = 1.0f};
    // Clamped before the conversion, since a budget meant as "no limit" fits more rounds than an i32 holds.
    auto const fit = cc::clamp(budget_seconds / seconds_per_full_round, 1.0, f64(max_rounds));
    return {.rounds = i32(fit), .fill = 1.0f};
}

void wavefront_pacing::reset()
{
    seconds_per_full_round = 0.0;
    fill = 1.0f;
    still_frames = 0;
    in_flight.clear();
}
} // namespace sv::impl
