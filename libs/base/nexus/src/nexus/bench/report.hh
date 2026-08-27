#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/fwd.hh>
#include <nexus/bench/result.hh>

namespace cc::rec
{
struct unit;
} // namespace cc::rec

// Turning results into something a person reads in five seconds.
//
// The whole point of this layer is that reading the number CARELESSLY still reads it correctly.
// A benchmark that prints 123.4571 ns is claiming seven digits it does not have, and the reader who compares two runs
// digit by digit concludes something changed when nothing did.

/// How a report is drawn, which is about the destination rather than about taste.
struct nx::bench::report_style
{
    /// Draw the unreliable digits muted instead of bracketing them.
    /// Strictly better where it is available, since it costs no punctuation at all.
    bool color = false;

    /// Emit a table that survives being pasted into a `.md` file.
    /// Benchmark numbers end up in docs routinely here, and that is where a clever delimiter goes wrong.
    bool markdown = false;

    /// Add the full statistics block under every row of a table.
    /// A single-loop report is always drawn in full, so this changes nothing there.
    bool verbose = false;

    isize width = 100;

    /// Colour resolved from the process's own streams, width from the terminal.
    [[nodiscard]] static report_style for_console();
};

namespace nx::bench
{
/// A value with the digits its uncertainty reaches marked as unreliable.
///
/// `1.83[4] ms` says the 4 is not resolved: the half-width of the interval reaches that decimal place.
/// Under `color` the same digits are drawn muted and no brackets appear at all.
///
/// **Brackets rather than parentheses, deliberately.**
/// `12(3)` is standard concise uncertainty notation meaning `12 ± 3`, where here it would mean `123 ± 2` — the same
/// string read two ways that differ by a factor of ten.
/// Tildes were the other candidate and lose to markdown, where `~4~` is subscript and `~~x~~` is strikethrough.
///
/// A value whose uncertainty reaches every digit it has is not a number with a wide interval; it is a failed
/// measurement, and it prints as `unstable` plus the raw interval.
[[nodiscard]] cc::string format_uncertain(f64 value, f64 half_width, cc::rec::unit const* unit, report_style const& style);

/// A value with its unit's prefix and symbol, and no uncertainty — for a count, a size, a rate.
[[nodiscard]] cc::string format_quantity(f64 value, cc::rec::unit const* unit);

/// The report for one benchmark's loops.
///
/// **One loop gets a block, several get a table**, because the two shapes buy different things.
/// A table exists to make rows comparable, and that costs columns; with one row there is nothing to compare and the
/// width is better spent on depth.
///
/// A trailing newline is included, and the string is ready to print.
[[nodiscard]] cc::string format_report(cc::string_view title, cc::span<result const> loops, report_style const& style);
} // namespace nx::bench
