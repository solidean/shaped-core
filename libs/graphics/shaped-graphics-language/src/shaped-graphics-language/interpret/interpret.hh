#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>

/// The abstract machine of the flat tree, run directly: what a tree MEANS, in either form.
///
/// Operands and arguments are evaluated left to right, each exactly once, and a `print` is the one effect besides the result.
/// It exists to compare two trees, so it is exact about order and indifferent to speed.
/// Its arithmetic is `f32` and wrapping `i32`; a target's own rounding is not modelled, and `normalize` takes its root by iteration.

enum class sgl::check::value_kind : sgl::u8
{
    none,
    scalar_float,
    scalar_int,
    boolean,
};

/// One leaf of a value, held as its bits so that equality is exact: a NaN equals itself and 0.0 differs from -0.0.
struct sgl::check::scalar
{
    value_kind kind = value_kind::none;
    u32 bits = 0;

    [[nodiscard]] static scalar of(f32 v);
    [[nodiscard]] static scalar of(i32 v);
    [[nodiscard]] static scalar of(bool v);

    [[nodiscard]] f32 as_float() const;
    [[nodiscard]] i32 as_int() const { return i32(bits); }
    [[nodiscard]] bool as_bool() const { return bits != 0; }

    constexpr bool operator==(scalar const&) const = default;
};

/// A value of any type as its scalars in field order, nested structs included; a `mat4` is 16 of them, column by column.
struct sgl::check::value
{
    type_id type = type_id::none;
    cc::vector<scalar> leaves;

    [[nodiscard]] bool operator==(value const& rhs) const
    {
        return type == rhs.type && ast::impl::is_equal(leaves, rhs.leaves);
    }
};

enum class sgl::check::run_status : sgl::u8
{
    ok,
    /// The run took more steps than `run_limits::fuel`.
    out_of_fuel,
    /// The end of the function, or of a block expression, was reached without a value.
    fell_off_the_end,
    /// The tree is not well formed or not well typed where the run came through; `outcome::detail` says how.
    type_error,
    /// A `var` was read before anything was assigned to it.
    uninitialized_read,
};

struct sgl::check::run_inputs
{
    /// The value of `locals[0]`.
    value parameter;
    /// Parallel to `flat_entry_point::bindings`: the members of each binding as one value, in member order.
    cc::vector<value> bindings;
};

struct sgl::check::run_limits
{
    /// One unit per statement, per expression node and per iteration.
    i64 fuel = 1'000'000;
};

struct sgl::check::outcome
{
    run_status status = run_status::ok;
    /// What the entry point returned; empty unless `status` is `ok`.
    value result;
    /// Every printed value, in the order the prints ran; kept up to the point a failed run stopped.
    cc::vector<value> trace;
    /// For a reader, and no part of what two runs are compared by.
    cc::string detail;

    /// Same status, same result, same trace.
    [[nodiscard]] bool operator==(outcome const& rhs) const
    {
        return status == rhs.status && result == rhs.result && ast::impl::is_equal(trace, rhs.trace);
    }
};

namespace sgl::check
{
/// `ok`, `out-of-fuel`, `fell-off-the-end`, `type-error`, `uninitialized-read`.
[[nodiscard]] cc::string_view to_string(run_status s);

/// How many scalars a value of `type` has; 0 for a type that has no value here.
[[nodiscard]] isize leaf_count_of(checked_module const& m, type_id type);

/// A value of `type` with every scalar zero, of the kind its place in the type says.
[[nodiscard]] value zero_value(checked_module const& m, type_id type);

/// Runs `e`, structured or core, on the abstract machine.
/// Total: a malformed tree is a `type_error`, a run without end is `out_of_fuel`, and nothing asserts.
/// Deterministic: equal arguments give equal outcomes.
[[nodiscard]] outcome interpret(checked_module const& m,
                                flat_entry_point const& e,
                                run_inputs const& inputs,
                                run_limits const& limits = {});

/// `ok 1.5` with one ` | print …` per printed value, for a failing test to show.
[[nodiscard]] cc::string dump(outcome const& o);
} // namespace sgl::check
