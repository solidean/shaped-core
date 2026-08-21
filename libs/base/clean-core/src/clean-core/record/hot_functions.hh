#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/string/string.hh>

// Where the time went, from the stacks a recording already carries.
//
// A sampled recording answers this without a viewer, which is what makes it usable from a test: a regression that
// moves a function from 3% to 40% of samples is an assertion, not something you have to go and look at.
//
// **Self time is what a profile is for.** A function high in TOTAL and low in SELF is a caller and rarely the problem.
//
// Symbolization happens here rather than during capture, and it is why this is analysis-side: resolving one address
// costs orders of magnitude more than the sample that recorded it.

namespace cc::rec
{
struct hot_function;
struct hot_report;
struct hot_options;
} // namespace cc::rec

struct cc::rec::hot_options
{
    /// Also fold in the stacks that log events captured.
    /// Off by default: those are wherever errors were logged, which is a different question and a much smaller sample.
    bool include_stacktrace_events = false;

    /// Drop anything below this share of self samples, as a ratio rather than a percentage.
    f64 min_self_ratio = 0;
};

/// One function, and how much of the profile landed in it.
struct cc::rec::hot_function
{
    /// The resolved name, or the module for a frame that has no symbols, or `<unknown>` for neither.
    ///
    /// Grouping an unresolved frame by its MODULE is deliberate: "38% of samples in a driver DLL" is actionable and a
    /// list of distinct addresses in it is not.
    cc::string function;

    /// A source location seen inside this function, and not its declaration.
    /// An aggregate covers many addresses, so this is where one of them was, kept because it is enough to navigate.
    cc::string file;
    i32 line = 0;

    /// Samples whose INNERMOST captured frame was here — the time spent in this function itself.
    isize self_samples = 0;

    /// Samples with this function anywhere in the stack, counted once each however often it recurs.
    isize total_samples = 0;

    f64 self_ratio = 0;
    f64 total_ratio = 0;
};

/// What the stacks in a recording add up to, ordered by self time.
struct cc::rec::hot_report
{
    /// Descending by self samples, then by total, then by name — so the order is stable enough to assert on.
    cc::vector<rec::hot_function> functions;

    /// Stacks folded in, which is the denominator of every ratio.
    isize sample_count = 0;

    /// Stacks whose innermost frame resolved to no module at all, so they count in `sample_count` and in nothing else.
    isize unresolved_samples = 0;

    [[nodiscard]] bool empty() const { return functions.empty(); }

    /// The entry for `function`, or null.
    /// The name must match what the symbolizer produced, so this is for asserting on a function you already named.
    [[nodiscard]] rec::hot_function const* find(cc::string_view function) const;

    /// The share of samples whose innermost frame was in `function`, or zero if it never was.
    [[nodiscard]] f64 self_ratio_of(cc::string_view function) const;

    /// **Inclusive counts only cover what was captured.**
    /// Sampling stops at the innermost open profiling scope, so a stack below one is deliberately short and the scope
    /// spans carry the rest.
    [[nodiscard]] cc::string to_string(isize max_rows = 20) const;
};

namespace cc::rec
{
/// Aggregates the recording's captured stacks by function.
///
/// Resolves against the recording's own module table where it has one, so a recording from another run — or from a
/// process that has since died — reports the names its own binaries had.
[[nodiscard]] rec::hot_report hot_functions(rec::recording const& r, rec::hot_options const& opts = {});
} // namespace cc::rec
