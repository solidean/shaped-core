#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <nexus/fuzz/fwd.hh>
#include <nexus/fuzz/run.hh>

namespace cc
{
struct random;
}

namespace nx::fuzz::impl
{
/// Stands in for a producer when the argument is a synthesized cc::random&.
inline constexpr int random_producer = -1;

/// One step of a program in producer (SSA) form: each argument names the step that produced it, rather than a slot.
/// That form is stable under deletion, so shrinking deletes steps and regenerates slot indices instead of patching them.
struct logical_step
{
    op_index op = op_index::invalid;
    u64 state = 0;
    bool result_must_be_true = false;
    cc::vector<int> arg_producers; // random_producer, or an index into the step list
};

[[nodiscard]] cc::vector<logical_step> derive_logical(fuzz_run const& run);

/// Rebuilds a slot-addressed program, giving every produced value a fresh appended slot.
/// Well-formed as long as every kept step's producers are kept too.
[[nodiscard]] fuzz_run regenerate(fuzz_machine const& m, cc::vector<logical_step> const& steps);

/// Shrinks a failing program as a state machine, so any driver can replay the candidates — synchronously or awaited.
///
///     auto shrink = minimizer(failing, rng);
///     for (auto candidate = shrink.next_candidate(); candidate.has_value(); candidate = shrink.next_candidate())
///         shrink.report(candidate.value().replay());
///     auto const minimal = shrink.result();
///
/// Every round tree-shakes first, then tries single-step removals in shuffled order, and adopts the first candidate that still fails.
/// It stops at a round where no candidate does.
/// report() must follow each next_candidate() exactly once, with that candidate's replay.
struct minimizer
{
    /// `rng` must outlive the minimizer; it only shuffles the removal order.
    minimizer(fuzz_run const& failing, cc::random& rng);

    [[nodiscard]] cc::optional<fuzz_run> next_candidate();
    void report(fuzz_run::replay_result const& r);

    [[nodiscard]] fuzz_run result() const;

private:
    enum class phase
    {
        round_start,
        removals,
        done,
    };

    void begin_removals();
    void adopt(cc::vector<logical_step> candidate, int failing_op);

    fuzz_machine const* _machine = nullptr;
    cc::random* _rng = nullptr;
    cc::vector<logical_step> _current;
    cc::vector<logical_step> _candidate;
    bool _awaiting_report = false;

    phase _phase = phase::round_start;
    cc::vector<int> _order; // the shuffled removal order of this round
    int _next_removal = 0;  // index into _order
};
} // namespace nx::fuzz::impl
