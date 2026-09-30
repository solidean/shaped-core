#pragma once

#include <shaped-graphics-language/builtins/registry.hh>

/// Registration is explicit: one function calls the topics in a fixed order.
/// So the generated prelude is deterministic, and no linker can drop a translation unit nobody names.
/// Until SGL has generics, a family of overloads is a C++ loop here that formats one signature per type.

namespace sgl::builtins
{
/// Every topic below, in the order the generated file shows them; `r` is not finalized.
void register_builtins(registry& r);

/// `float`, `int`, `uint` and `bool` with their vectors, and `mat4`; first, since every signature names them.
void register_types(registry& r);
/// Arithmetic, comparisons and the scalar functions of `float`, `int`, `uint` and `bool`.
void register_scalar_math(registry& r);
/// The componentwise families of the vectors, and what `pos3` and `vec3` mean together.
void register_vector_math(registry& r);
/// `mat4` times a matrix, a position, a direction and a plain four-vector.
void register_transforms(registry& r);
/// `x as T` between `float`, `int` and `uint` of one width, as operator functions of `as`.
void register_conversions(registry& r);
/// The texture and image methods, for every shape.
void register_textures(registry& r);
/// `&`, `|`, `^`, `~`, `<<` and `>>` of `int`, `uint` and their vectors.
void register_bit_math(registry& r);
/// Transcendentals, rounding, derivatives, geometry, integer bit functions, packing and reinterpreting bits.
void register_math(registry& r);
/// The barriers of a compute workgroup.
void register_sync(registry& r);
/// The steps of an inline ray query, `ray_flags`, and the emulated trace's view of sg's acceleration pool.
void register_raytracing(registry& r);
} // namespace sgl::builtins

/// What the topic files share.
namespace sgl::builtins::impl
{
/// `@pure @operator("op") fun name(a: lhs, b: rhs) -> result`, written `a op b` by every target.
void add_infix(registry& r,
               cc::string_view op,
               cc::string_view name,
               cc::string_view lhs,
               cc::string_view rhs,
               cc::string_view result,
               evaluator evaluate,
               undefined_check undefined_when = nullptr);

/// `@pure @operator("op") fun name(a: lhs, b: rhs) -> result`, with a spelling of its own.
void add_operator(registry& r,
                  cc::string_view op,
                  cc::string_view name,
                  cc::string_view lhs,
                  cc::string_view rhs,
                  cc::string_view result,
                  evaluator evaluate,
                  spelling write,
                  undefined_check undefined_when = nullptr);

/// `/` and `%` of `int`, `uint` and their vectors, componentwise, each reading its kind off its leaves.
/// Both truncate toward zero, as every target does: `-7 / 2 == -3` and `-7 % 3 == -1`.
void divide_integers(cc::span<check::scalar const> in, cc::vector<check::scalar>& out);
void remainder_integers(cc::span<check::scalar const> in, cc::vector<check::scalar>& out);
/// A zero divisor, and the one quotient that overflows `int`: the values no target agrees on.
[[nodiscard]] cc::string_view integer_division_undefined(cc::span<check::scalar const> in);

/// What WGSL refuses where every operand is constant, componentwise (CHK-312).
/// A constant int is folded exactly, so a value outside the ints is an error there; a uint wraps, as WGSL folds it.
[[nodiscard]] cc::string_view sum_unrepresentable(cc::span<check::scalar const> in);
[[nodiscard]] cc::string_view difference_unrepresentable(cc::span<check::scalar const> in);
[[nodiscard]] cc::string_view product_unrepresentable(cc::span<check::scalar const> in);
/// The most negative int, negated or made absolute.
[[nodiscard]] cc::string_view negation_unrepresentable(cc::span<check::scalar const> in);
/// An int shifted left past its sign, or a uint past its top bit.
[[nodiscard]] cc::string_view shift_left_unrepresentable(cc::span<check::scalar const> in);
/// A negative int converted to uint.
[[nodiscard]] cc::string_view negative_as_uint(cc::span<check::scalar const> in);

/// `a % b` of floats, componentwise: `a - b * trunc(a / b)`, WGSL's definition, which HLSL's `fmod` shares.
void remainder_floats(cc::span<check::scalar const> in, cc::vector<check::scalar>& out);
/// `a % b` in HLSL and WGSL, `fmod(a, b)` in MSL, which has no `%` of floats.
[[nodiscard]] spelling float_remainder();

/// `@pure @operator("-") fun name(x: type) -> type`, written `-x` by every target.
void add_negate(registry& r, cc::string_view name, cc::string_view type, evaluator evaluate);

/// `@pure fun name(p0: t0, p1: t1, …) -> result`, written as a call of `write.text` or of `name`.
/// `parameters` alternates names and types: `{"a", "float3", "b", "float3"}`.
void add_function(registry& r,
                  cc::string_view name,
                  cc::span<cc::string_view const> parameters,
                  cc::string_view result,
                  evaluator evaluate,
                  spelling write = {},
                  cc::string_view doc = {},
                  undefined_check undefined_when = nullptr);

/// `clamp(x, low, high)` whose `low` is above its `high`, componentwise and of any kind: MSL leaves it undefined, and
/// WGSL refuses it where both are constant.
[[nodiscard]] cc::string_view clamp_undefined(cc::span<check::scalar const> in);

/// The suffix an operator function of `type` carries in its name: none for `float`, `_int`, `_color` for `float3`.
/// An operator function is found through its operator alone, so the name is documentation and shows in a dump.
[[nodiscard]] cc::string suffix_of(cc::string_view type);
} // namespace sgl::builtins::impl
