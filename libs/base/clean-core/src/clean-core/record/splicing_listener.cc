#include "splicing_listener.hh"

using namespace cc::primitive_defines;

namespace
{
/// Whether a block holds samples and nothing else.
///
/// That is exactly the shape `spliced_samples` gives an unplaced sample: the block it came from is rebuilt without
/// its samples, and whatever could not be placed is handed back in a block of its own.
/// So this identifies what is still looking for a home, without the splice having to report it.
[[nodiscard]] bool is_leftover_samples(cc::rec::recorded_block const& b)
{
    auto const v = b.view();

    auto any = false;
    for (auto it = v.begin(); it != v.end(); ++it)
    {
        if ((*it).kind() != cc::rec::event_kind::sample)
            return false;
        any = true;
    }
    return any;
}

[[nodiscard]] isize count_samples(cc::rec::recording const& r)
{
    return r.count_of_kind(cc::rec::event_kind::sample);
}
} // namespace

void cc::rec::splicing_listener::on_chunk(rec::chunk_view const& view)
{
    _pending.append(view);
}

void cc::rec::splicing_listener::release(rec::recording const& blocks)
{
    for (auto const& b : blocks.blocks())
        _downstream.on_chunk(b.view());
}

void cc::rec::splicing_listener::on_batch_end()
{
    if (_pending.empty() && _held.empty())
    {
        _downstream.on_batch_end();
        return;
    }

    // Held first, so a carried sample is offered to this batch's blocks in the order it was taken.
    rec::recording combined;
    combined.append(_held);
    combined.append(_pending);

    auto const before = count_samples(combined);

    auto const spliced = combined.spliced_samples();

    rec::recording forward;
    rec::recording leftover;
    for (auto const& b : spliced.blocks())
    {
        if (is_leftover_samples(b))
            leftover.append_block(b);
        else
            forward.append_block(b);
    }

    auto const still_unplaced = count_samples(leftover);
    _spliced += before - still_unplaced;

    release(forward);

    _pending.clear();
    _held = cc::move(leftover);

    // The cap is what bounds the delay and the memory both.
    // Forwarding an unplaceable sample is not a loss — it keeps the position it was recorded at, which is what an
    // offline splice does with one it cannot place either.
    if (!_held.empty())
    {
        ++_rounds_held;
        if (_rounds_held >= _opts.max_hold_batches)
        {
            _unplaced += count_samples(_held);
            release(_held);
            _held.clear();
            _rounds_held = 0;
        }
    }
    else
        _rounds_held = 0;

    _downstream.on_batch_end();
}

void cc::rec::splicing_listener::flush()
{
    on_batch_end();

    if (_held.empty())
        return;

    _unplaced += count_samples(_held);
    release(_held);
    _held.clear();
    _rounds_held = 0;
}
