#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>
#include <shaped-graphics-language/interpret/scalar.hh>

/// The abstract machine of the flat tree, run directly: what a tree MEANS, in either form.
///
/// Operands and arguments are evaluated left to right, each exactly once, and a `print` is the one effect besides the result.
/// It exists to compare two trees, so it is exact about order and indifferent to speed.
/// Its arithmetic is `f32` and wrapping `i32`; a target's own rounding is not modelled, and `normalize` takes its root by iteration.

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

/// One `buffer[T]` member of a binding: its elements' scalars, element after element.
struct sgl::check::buffer_contents
{
    symbol_id binding = symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;
    cc::vector<scalar> leaves;

    [[nodiscard]] bool operator==(buffer_contents const& rhs) const
    {
        return binding == rhs.binding && member == rhs.member && ast::impl::is_equal(leaves, rhs.leaves);
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
    /// An `assert` was false; the run stopped there (EVAL-76).
    assertion_failed,
};

struct sgl::check::run_inputs
{
    /// The value of `locals[0]`.
    value parameter;
    /// Parallel to `flat_entry_point::bindings`: the members of each binding as one value, in member order.
    /// A buffer member has no scalars there; its contents are `buffers`.
    cc::vector<value> bindings;
    /// What every buffer the tree reads or writes holds when the run starts; one it names and this lacks is a type error.
    cc::vector<buffer_contents> buffers;
};

struct sgl::check::run_limits
{
    /// One unit per statement, per expression node and per iteration.
    i64 fuel = 1'000'000;
    /// Checks and asserts run, as the structured form means (EVAL-75).
    /// False skips each with its body, as the core form does, which is what a comparison of the two forms wants.
    bool run_checks = true;
    /// Failures past this many are counted in `outcome::failures_dropped` and not kept.
    i32 max_failures = 8;
};

/// One check or `assert` that was false where it ran.
struct sgl::check::check_failure
{
    /// A position in the tree's `check_sites`.
    i32 site = -1;
    /// Parallel to the site's nodes: the value each node's `var` held, and whether the node ran at all.
    cc::vector<value> values;
    cc::vector<bool> is_evaluated;
    /// Parallel to the site's `loop_variables`.
    cc::vector<value> loop_values;

    [[nodiscard]] bool operator==(check_failure const& rhs) const
    {
        return site == rhs.site && ast::impl::is_equal(values, rhs.values)
            && ast::impl::is_equal(is_evaluated, rhs.is_evaluated) && ast::impl::is_equal(loop_values, rhs.loop_values);
    }
};

struct sgl::check::outcome
{
    run_status status = run_status::ok;
    /// What the entry point returned; empty unless `status` is `ok`.
    value result;
    /// Every printed value, in the order the prints ran; kept up to the point a failed run stopped.
    cc::vector<value> trace;
    /// Every buffer the run stored to, as the run left it: a store is an effect, so two runs are compared by these too.
    cc::vector<buffer_contents> buffers;
    /// For a reader, and no part of what two runs are compared by.
    cc::string detail;
    /// Every check and `assert` that was false, up to `run_limits::max_failures`, in the order they ran.
    cc::vector<check_failure> failures;
    /// How many checks ran, whatever they found, `assert`s included (EVAL-76); a test whose run ran none has checked nothing.
    i32 checks_run = 0;
    /// How many of `checks_run` were `assert`s.
    i32 asserts_run = 0;
    /// How many failures `max_failures` left out.
    i32 failures_dropped = 0;

    /// Same status, same result, same trace, same buffers.
    [[nodiscard]] bool operator==(outcome const& rhs) const
    {
        return status == rhs.status && result == rhs.result && ast::impl::is_equal(trace, rhs.trace)
            && ast::impl::is_equal(buffers, rhs.buffers);
    }
};

namespace sgl::check
{
/// `ok`, `out-of-fuel`, `fell-off-the-end`, `type-error`, `uninitialized-read`, `assertion-failed`.
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

/// `ok 1.5` with one ` | print …` per printed value, then one ` | buffer …` per buffer, for a failing test to show.
[[nodiscard]] cc::string dump(outcome const& o);
} // namespace sgl::check
