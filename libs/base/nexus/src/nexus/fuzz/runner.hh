#pragma once

#include <clean-core/container/vector.hh>
#include <nexus/fuzz/executed_operation.hh>
#include <nexus/fuzz/fwd.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/fwd.hh>

namespace cc
{
struct random;
}

/// Generates a single fuzz program: on each step it rolls a type-correct operation, biased toward operations that still owe executions (execute_at_least).
/// The fallback runs any eligible operation, to manufacture missing prerequisite values.
/// A non-progress guard aborts a pathological setup, such as a precondition that can never be satisfied.
struct nx::fuzz::fuzz_runner
{
    fuzz_runner(fuzz_machine const& machine, cc::random& rng);

    /// True while some operation still owes required executions.
    [[nodiscard]] bool should_continue() const;

    /// Rolls the next step into `out`. Returns false if no eligible step could be produced (the run
    /// then ends without being considered a failure).
    [[nodiscard]] bool create_next_execution(fuzz_machine::state const& s, executed_operation& out);

private:
    [[nodiscard]] bool eligible(op_index op, fuzz_machine::state const& s) const;
    [[nodiscard]] bool try_roll(op_index op, fuzz_machine::state const& s, executed_operation& out) const;

    static constexpr int max_eligible_recheck = 100;
    static constexpr int max_fallback_check = 100;
    static constexpr int max_consecutive_non_progress = 10000;

    fuzz_machine const& _machine;
    cc::random& _rng;
    cc::vector<int> _exec_count;
    int _non_progress = 0;
};
