#pragma once

#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/interpret/interpret.hh>

/// A bounding run's ranges (`run_limits::bounds_floats`): what a GPU may give for a value under an entry point's
/// `@float` level, as EVAL-6 licenses each operation to depart from the exact one.

namespace sgl::check::impl
{
/// What a level licenses that a bounding run models; a builtin's own precision is not among them.
struct float_licenses
{
    /// A half operation's result kept at a float's precision rather than rounded to a half (EVAL-96); a conversion
    /// still rounds.
    bool keeps_half_at_float = false;
    /// A float product left unrounded, as a fused multiply-add keeps it.
    bool contracts = false;
    /// A denormal argument or result taken as zero.
    bool flushes_denormals = false;
    /// A sum added in any order: what `+` and `-` chain together, `scalar::terms`.
    bool reassociates = false;

    [[nodiscard]] bool is_any() const { return keeps_half_at_float || contracts || flushes_denormals || reassociates; }
};

[[nodiscard]] float_licenses licenses_of(float_level level);

/// Gives each of `results` the range a GPU may give for it, where `results` are what `record` gives for `args`.
/// `args` are the call's arguments' leaves as the run holds them, ranges included; `results` are its exact leaves,
/// narrowed to the result's kinds.
///
/// A builtin is evaluated at the ends of its ranged arguments and the hull taken: every combination of ends where
/// there are few, and one argument at a time, the rest exact, where there are many.
/// The first is exact for what is monotone in each argument, as arithmetic and conversions are; the second is exact
/// for what is linear, and both are off by second order elsewhere, which ranges a few ulps wide make negligible.
/// Arguments of one origin are one value and move together; a ranged result's origin numbers the builtin and its
/// arguments, so one expression computed twice is one value.
/// A non-float result that differs at some end is unbounded, as is every result of an unbounded argument.
void bound_call(builtins::function_record const& record,
                cc::span<scalar const> args,
                float_licenses licenses,
                cc::span<scalar> results);

/// Widens what `into` stored and returned to hold what `other` did too, both runs of `inputs` that went different
/// ways through the conditions their ranges left open.
/// A float or a half leaf takes the hull of both; any other leaf the two runs disagree on is unbounded.
/// A buffer or an image only one of them stored to is joined with what `inputs` held.
void join_into(outcome& into, outcome const& other, run_inputs const& inputs);
} // namespace sgl::check::impl
