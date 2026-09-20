#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// Everything a `@builtin` declaration of the prelude may stand for.
/// The declaration's NAME is the key, so the prelude says what a builtin looks like and this says that it exists.
/// An emitter switches over these; it never reads a builtin's name.
enum class sgl::check::builtin : sgl::u8
{
    /// Not a builtin.
    none,

    /// `float`; the other names are their own spelling.
    scalar_float,
    float3,
    float4,
    vec3,
    pos3,
    hpos4,
    mat4,
    /// `int`: 32 bits, signed, and its arithmetic wraps.
    scalar_int,
    /// `bool`.
    boolean,

    normalize,
    dot,
    saturate,
    transform_position,
    transform_direction,
    scale_color,
    multiply,
    add,
    subtract,
    less,
    equal,
    add_int,
    subtract_int,
    multiply_int,
    less_int,
    equal_int,

    divide,
    negate,
    less_equal,
    greater,
    greater_equal,
    not_equal,
    less_equal_int,
    greater_int,
    greater_equal_int,
    not_equal_int,
    add_color,
    multiply_color,
    add_vec3,
    subtract_vec3,
    scale_vec3,
    min,
    max,
    clamp,
    mix,
    length,
    abs,
};

namespace sgl::check
{
/// `none` for a name the compiler does not know.
[[nodiscard]] builtin builtin_of(cc::string_view name);

/// The name a `@builtin` declaration carries; empty for `none`.
[[nodiscard]] cc::string_view to_string(builtin b);

/// True for a builtin a `struct` declares, false for one a `fun` declares.
/// `b` must not be `none`.
[[nodiscard]] bool is_type(builtin b);
} // namespace sgl::check
