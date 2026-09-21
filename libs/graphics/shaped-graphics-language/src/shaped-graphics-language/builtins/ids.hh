#pragma once

#include <shaped-graphics-language/fwd.hh>

/// A builtin is named by its position in the registry, one id type per array, like every other tree here.
/// `none` is what a declaration of the program's own carries.

/// A position in `builtins::registry::functions`: one OVERLOAD, so `dot(vec3, vec3)` and `dot(float3, float3)` are two.
enum class sgl::builtin_id : sgl::i32
{
    none = -1
};

/// A position in `builtins::registry::types`.
enum class sgl::builtin_type_id : sgl::i32
{
    none = -1
};

namespace sgl
{
[[nodiscard]] constexpr bool is_valid(builtin_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(builtin_type_id id)
{
    return i32(id) >= 0;
}

/// `id` must be valid.
[[nodiscard]] constexpr isize index_of(builtin_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(builtin_type_id id)
{
    return isize(id);
}
} // namespace sgl
