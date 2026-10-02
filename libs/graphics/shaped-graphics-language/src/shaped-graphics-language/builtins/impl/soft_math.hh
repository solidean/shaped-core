#pragma once

#include <shaped-graphics-language/fwd.hh>

// The transcendental functions the interpreter evaluates builtins with, written out rather than taken from a libm.
//
// A host's libm differs from another's in the last bit, and two runs of one tree must meet the same bits on every
// machine, since a test's verdict and the editor's marks are compared across them.
// So each is plain f64 arithmetic: a range reduction and a series, correct to a few ULP of f32, which is what a
// result is rounded to and what no GPU promises to match anyway.

namespace sgl::builtins::impl
{
[[nodiscard]] f64 soft_sqrt(f64 x);
[[nodiscard]] f64 soft_exp(f64 x);
/// -inf at 0, and NaN below it.
[[nodiscard]] f64 soft_log(f64 x);
[[nodiscard]] f64 soft_sin(f64 x);
[[nodiscard]] f64 soft_cos(f64 x);
[[nodiscard]] f64 soft_atan(f64 x);
/// The angle of (x, y) in -pi..pi, and 0 at the origin.
[[nodiscard]] f64 soft_atan2(f64 y, f64 x);

/// Integral parts; NaN, an infinity and anything past 2^52 are returned as they are.
[[nodiscard]] f64 soft_floor(f64 x);
[[nodiscard]] f64 soft_trunc(f64 x);
/// Ties to even, as WGSL's `round` and HLSL's do.
[[nodiscard]] f64 soft_round(f64 x);

/// The half closest to `x`, ties to even, as its bits; what `pack_half2x16` stores and what a `half` holds.
/// TEMPORARY: clean-core has no half type yet, and typed-geometry's `tg::f16` is out of reach, since SGL depends on
/// clean-core alone; docs/lower-library-gaps.md records the gap.
[[nodiscard]] u32 half_bits_of(f32 x);
/// The same from an f64, rounded once: a literal's exact value, which an f32 in between would round twice.
[[nodiscard]] u32 half_bits_of(f64 x);
/// Exact, since every half is an f32.
[[nodiscard]] f32 float_of_half_bits(u32 h);
} // namespace sgl::builtins::impl
