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
/// DEBUG: the stand-ins for sampling, loading and storing texels, until textures have methods.
void register_textures(registry& r);
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
               evaluator evaluate);

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
                  cc::string_view doc = {});

/// The suffix an operator function of `type` carries in its name: none for `float`, `_int`, `_color` for `float3`.
/// An operator function is found through its operator alone, so the name is documentation and shows in a dump.
[[nodiscard]] cc::string suffix_of(cc::string_view type);
} // namespace sgl::builtins::impl
