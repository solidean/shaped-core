#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>

namespace sg::impl
{
void transfer_scheduler::set_stream_ratio(float ratio)
{
    CC_ASSERT(ratio >= 0.0f && ratio <= 1.0f, "stream ratio must be in [0, 1]");
    _stream_ratio = ratio;
}

void transfer_scheduler::set_aging_factor(float per_second)
{
    CC_ASSERT(per_second >= 0.0f, "aging factor must be non-negative");
    _aging_factor = per_second;
}

void transfer_scheduler::set_window_bytes(isize bytes)
{
    CC_ASSERT(bytes > 0, "window size must be positive");
    _window_bytes = bytes;
}

void transfer_scheduler::begin_window()
{
    // Owed bandwidth is what decides, so an idle stream never costs async a window and a starved one gets a whole
    // window rather than a fraction too small to be useful.
    _window_primary = _stream_deficit > 0 ? transfer_flavor::streaming : transfer_flavor::async;
}

void transfer_scheduler::on_window_submitted(isize async_bytes, isize stream_bytes)
{
    CC_ASSERT(async_bytes >= 0 && stream_bytes >= 0, "window byte counts must be non-negative");

    isize const total = async_bytes + stream_bytes;
    _stream_deficit += isize(double(_stream_ratio) * double(total)) - stream_bytes;

    // A long stretch with no streaming work must not bank credit it would then spend all at once.
    // One window's worth is the natural bound: enough to guarantee the next window, never more.
    if (_window_bytes > 0)
        _stream_deficit = cc::clamp(_stream_deficit, -_window_bytes, _window_bytes);
}

cc::optional<isize> transfer_scheduler::pick_next(cc::span<transfer_candidate const> candidates) const
{
    if (candidates.empty())
        return {};

    transfer_flavor const other
        = _window_primary == transfer_flavor::async ? transfer_flavor::streaming : transfer_flavor::async;

    if (auto const primary = pick_of_flavor(candidates, _window_primary); primary.has_value())
        return primary;
    return pick_of_flavor(candidates, other);
}

/// Whether `i` is the head of its family — no same-family job was submitted before it.
/// An INELIGIBLE head still blocks its family, which is what keeps same-destination transfers composing.
[[nodiscard]] static bool is_family_head(cc::span<transfer_candidate const> candidates, isize i)
{
    for (isize j = 0; j < candidates.size(); ++j)
        if (j != i && candidates[j].family == candidates[i].family && candidates[j].sequence < candidates[i].sequence)
            return false;
    return true;
}

cc::optional<isize> transfer_scheduler::pick_of_flavor(cc::span<transfer_candidate const> candidates,
                                                       transfer_flavor flavor) const
{
    // Linear, because aging makes an effective priority change continuously and any sorted structure goes stale.
    // One scan per window is nothing next to the memcpy and the GPU copy the window then performs.
    cc::optional<isize> best;
    float best_effective = 0.0f;
    for (isize i = 0; i < candidates.size(); ++i)
    {
        if (!candidates[i].eligible || candidates[i].flavor != flavor || !is_family_head(candidates, i))
            continue;

        if (flavor == transfer_flavor::async) // FIFO: async carries the strong guarantee and has no priority
        {
            if (!best.has_value() || candidates[i].sequence < candidates[best.value()].sequence)
                best = i;
            continue;
        }

        // Highest effective priority, ties broken by submission order.
        // FIFO within a tier and not round-robin, because a stream is only worth anything once it COMPLETES:
        // ten half-finished textures are worth nothing, five finished ones are worth five.
        float const effective = float(candidates[i].priority) + _aging_factor * candidates[i].age_seconds;
        if (!best.has_value() || effective > best_effective
            || (effective == best_effective && candidates[i].sequence < candidates[best.value()].sequence))
        {
            best = i;
            best_effective = effective;
        }
    }
    return best;
}
} // namespace sg::impl
