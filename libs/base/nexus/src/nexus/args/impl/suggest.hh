#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/fwd.hh>

// Did-you-mean, and nothing more: a suggestion is offered, never applied.
// Silently correcting a mistyped flag is how a script quietly does the wrong thing for a year.

namespace nx::impl
{
/// Levenshtein distance, abandoned once it provably exceeds `max` so a long candidate list stays cheap.
/// Returns `max + 1` for anything further apart than that.
[[nodiscard]] isize edit_distance(cc::string_view a, cc::string_view b, isize max);

/// The single closest candidate to `token`, or empty when nothing is close enough to be worth guessing.
///
/// Closeness is length-relative — one edit is a lot for `-j` and very little for `--parallelism` — with a
/// bonus for a candidate that `token` is a prefix or substring of, which is what makes `--out` suggest
/// `--output` even though three insertions is otherwise far.
[[nodiscard]] cc::string best_suggestion(cc::string_view token, cc::span<cc::string const> candidates);

} // namespace nx::impl
