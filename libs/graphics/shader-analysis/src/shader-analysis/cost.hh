#pragma once

#include <clean-core/string/string_view.hh>
#include <shader-analysis/fwd.hh>

// What a module would cost a GPU that has all its data at hand: the issue slots its operations take, as often as a wave
// runs them, each class of operation at its own rate.
//
// AN ESTIMATE FOR COMPARING TWO MODULES OF ONE ENTRY POINT, as if both were compute bound.
// Memory is left out: two routes of one entry point read and write the same footprint, so it moves neither alike.
// Each operation counts its lanes, a `float3`'s add three, which is what a GPU that works on scalars issues.
// How often it runs is `impl::frequencies`: a loop its stated trips or `k_assumed_loop_trips`, an arm of a branch the wave
// shares at its share, and both arms of one it does not.
//
//   auto const m = shan::analyze(module).value();
//   shan::compute_cost(m.total, shan::default_cost_model());   // in lane-cycles of an fma

/// What an operation spends of a GPU's issue slots.
enum class shan::cost_class : shan::u8
{
    /// A register's move: building or taking apart a composite, a phi, a copy, an address a driver folds.
    free,
    /// Full rate: a float add, multiply or fma, an integer add or bit operation, a compare, a select, a min or max.
    alu,
    /// A 32-bit integer multiply, which most GPUs issue at a fraction of the full rate.
    int_multiply,
    /// An integer division or remainder, which no GPU has a unit for: a sequence of multiplies.
    int_divide,
    /// A conversion between an integer and a float, or between float widths.
    conversion,
    /// A special function unit's: a reciprocal, a square root and its reciprocal, exp, log, sin, cos.
    transcendental,
    /// A read or a write of memory or an image, an atomic, a barrier, a trace: what this model leaves out.
    memory,
    /// A branch, a return, a call.
    control,
};

/// How much each class costs a lane, in issue slots of an fma.
struct shan::cost_model
{
    cc::string_view name;
    f64 per_lane[k_cost_class_count] = {};
    /// What an operation the whole wave shares costs against one that varies, which a scalar unit may run for the wave.
    f64 uniform_factor = 1.0;
};

namespace shan
{
/// The name `to_text` gives a class: `transcendental`.
[[nodiscard]] cc::string_view name_of(cost_class c);

/// Rates as GPUs document them, before any calibration: transcendentals and integer multiplies at a quarter of the full
/// rate, an integer division as a dozen multiplies, a branch as one slot.
[[nodiscard]] cost_model const& default_cost_model();

/// What `f` would spend if compute bound, in lane-cycles of an fma: each class's executed lanes at its rate.
[[nodiscard]] f64 compute_cost(function_metrics const& f, cost_model const& model);
} // namespace shan
