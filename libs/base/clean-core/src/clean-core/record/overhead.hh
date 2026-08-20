#pragma once

#include <clean-core/record/fwd.hh>

// What the recorder costs, measured rather than guessed.
//
// The whole design bets that annotation which is cheap is annotation that stays in the code.
// A bet like that is worth nothing without a number, and a number that came from a blog post about someone else's
// machine is worth less than nothing — so this measures the machine in front of you.
//
// The model is deliberately a straight line, `fixed + per_byte * payload`.
// It is not a claim that a recording site is linear; it is a claim that the two things it does — a constant amount of
// bookkeeping, and a copy — are the two terms worth fitting, and that anything else is either rare enough to measure
// itself (a stacktrace carries its own end timestamp) or already reported as a cold-path event.

/// What one recording site costs on this machine.
struct cc::rec::overhead_model
{
    /// Cycles a zero-payload event costs: the gate, the timestamp, the bounds check and the publish.
    f64 fixed_cycles = 0;

    /// Additional cycles per payload byte.
    f64 cycles_per_byte = 0;

    /// Cycles a DISABLED site costs — one load through the domain and a test.
    /// The number the "leave it in the code forever" argument actually rests on.
    f64 disabled_cycles = 0;

    /// False for the compile-time default, true once something measured it.
    /// An estimate built on a default is a guess wearing a number, and callers deserve to know which they have.
    bool is_measured = false;
};

namespace cc::rec
{
/// Measures the write path on this machine and returns the fit.
///
/// Takes a few milliseconds, and reports the MINIMUM per-event cost across several rounds rather than the mean:
/// a scheduling hiccup can only ever make a sample look slower, so the minimum is the honest estimate of the cost
/// and the mean is an estimate of the cost plus the machine's mood.
///
/// **This records into the live system**, because that is the only path worth measuring.
/// Its samples sit in the cc.record domain, so a listener that does not want them can filter them out; a caller who
/// wants a clean capture should measure before it starts, not during.
/// Requires the system to be initialized, and returns an unmeasured model when it is not.
[[nodiscard]] rec::overhead_model measure_overhead();

/// The model in effect.
/// A conservative compile-time default until something calls measure_overhead or set_overhead.
[[nodiscard]] rec::overhead_model const& overhead();

/// Installs a model, for a caller that measured once and wants it applied to later recordings.
void set_overhead(rec::overhead_model const& model);
} // namespace cc::rec
