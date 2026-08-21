#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/recording.hh>

// Resolving a captured stack, whether or not it was interned.
//
// A sampler at a kilohertz across twenty threads writes megabytes a second of return addresses, and nearly all of them
// repeat — the same stack, sampled again a millisecond later.
// So a deep stack is written ONCE under an id, and the samples after it carry the id instead.
//
// **A consumer should never read the frames field directly**, because a sample carries either shape and which one is
// the sampler's decision, not the reader's.
// Going through a table is what makes that decision invisible.

namespace cc::rec
{
struct stack_table;
} // namespace cc::rec

/// The interned stacks in one recording, and how to get frames out of any sample.
///
/// Built by one scan, so build it once and keep it for the whole analysis rather than per event.
/// **Only describes the recording it was built from** — ids are unique within a sampling run and mean nothing across
/// recordings.
struct cc::rec::stack_table
{
    stack_table() = default;
    explicit stack_table(rec::recording const& r);

    /// The frames behind an event, whichever way it stored them.
    /// Empty for an event that carries no stack at all, and for an interned id this table has no definition for.
    [[nodiscard]] cc::vector<u64> frames_of(rec::event_view const& e) const;

    /// The frames behind an id, or empty if it was never defined here.
    [[nodiscard]] cc::span<u64 const> frames_of_id(u64 id) const;

    /// How many distinct stacks were interned.
    [[nodiscard]] isize size() const { return _stacks.size(); }

    [[nodiscard]] bool empty() const { return _stacks.empty(); }

    /// Addresses this table holds, which is what interning traded a repeat for.
    [[nodiscard]] isize total_frames() const;

private:
    cc::map<u64, cc::vector<u64>> _stacks;
};
