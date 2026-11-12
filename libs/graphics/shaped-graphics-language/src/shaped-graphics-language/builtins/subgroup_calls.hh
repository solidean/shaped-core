#pragma once

#include <shaped-graphics-language/fwd.hh>

/// Which subgroup operation a record is, as its `spelling::data` holds it, for the writers and the interpreter.
/// The order is `register_subgroups.cc`'s table's.
enum class sgl::builtins::subgroup_op : sgl::u8
{
    all,
    any,
    ballot,
    add,
    mul,
    min,
    max,
    bit_and,
    bit_or,
    bit_xor,
    exclusive_add,
    exclusive_mul,
    inclusive_add,
    inclusive_mul,
    broadcast,
    broadcast_first,
    shuffle,
    shuffle_xor,
    shuffle_up,
    shuffle_down,
    quad_swap_x,
    quad_swap_y,
    quad_swap_diagonal,
    quad_broadcast,
};
