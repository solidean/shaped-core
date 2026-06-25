#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fuzz/executed_operation.hh>
#include <nexus/fuzz/regression_dialect.hh>

namespace cc
{
struct random;
}

namespace nx::fuzz
{
struct fuzz_machine;

/// A recorded fuzz program: the machine it ran against plus the linear list of executed steps.
/// A run is a value type, cheap to copy, which is what makes minimization (which probes many
/// derived candidate runs) practical.
struct fuzz_run
{
    fuzz_machine const* machine = nullptr;
    cc::vector<executed_operation> operations;

    struct replay_result
    {
        bool invalid_precondition = false; // a candidate became impossible (precondition no longer holds)
        int failing_op = -1;               // index of the first failing step, or -1 if the run passed

        [[nodiscard]] bool is_failing() const { return failing_op >= 0; }
    };

    /// Re-executes the recorded steps in order against a fresh state.
    [[nodiscard]] replay_result replay() const;

    /// Shrinks a failing run to a (locally) minimal one that still fails: dead-code tree-shaking
    /// followed by shuffled single-step removal, each candidate validated by replay, to a fixpoint.
    [[nodiscard]] fuzz_run minimize(cc::random& rng) const;

    /// Renders the run as copy-pasteable C++ regression code using `test_var` as the fuzz_test handle.
    [[nodiscard]] cc::string emit_regression(cc::string_view test_var, regression_dialect const& dialect) const;
};
} // namespace nx::fuzz
