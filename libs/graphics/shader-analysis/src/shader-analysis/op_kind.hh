#pragma once

#include <clean-core/string/string_view.hh>
#include <shader-analysis/fwd.hh>

// What an operation is, coarsely enough to compare two modules of one entry point by.
// A kind is about what a GPU spends on it, so a value moved between composites is `composite`, not arithmetic.

enum class shan::op_kind : shan::u8
{
    float_arithmetic,
    int_arithmetic,
    /// What an extended instruction set computes: `GLSL.std.450`'s sqrt, exp, fma, normalize.
    extended,
    conversion,
    compare,
    /// `select`, and the logical operations on bools.
    logic,
    /// Building, taking apart and shuffling composites, and copying a value.
    composite,
    /// An address: an access chain, an array length.
    address,
    load,
    store,
    /// A variable: memory of its own, which a driver may or may not keep in registers.
    variable,
    image,
    atomic,
    barrier,
    subgroup,
    derivative,
    ray,
    phi,
    /// A branch, a merge, a label, a return, a call.
    control,
    /// Debug info, names, decorations, types and constants: what states rather than runs.
    declaration,
    other,
};

namespace shan
{
inline constexpr isize k_op_kind_count = isize(op_kind::other) + 1;

/// The name `to_text` and `diff` give a kind: `float_arithmetic`.
[[nodiscard]] cc::string_view name_of(op_kind kind);

/// What a SPIR-V opcode is.
[[nodiscard]] op_kind spirv_kind_of(u16 opcode);

/// A rough latency of a kind, in cycles of an add, for the critical path: memory and images far above arithmetic.
/// No GPU's; the point is an order between kinds, for comparing two modules of one entry point.
[[nodiscard]] i32 latency_of(op_kind kind);
} // namespace shan
