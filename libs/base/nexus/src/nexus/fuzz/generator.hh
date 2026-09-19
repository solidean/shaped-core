#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <nexus/fuzz/executed_operation.hh>
#include <nexus/fuzz/fwd.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/fuzz/run.hh>
#include <nexus/fuzz/runner.hh>

namespace nx::fuzz::impl
{
/// Generates one fuzz program as a state machine, so any driver can run its steps — synchronously or awaited.
///
///     auto gen = generator(machine, seed);
///     for (auto step = gen.next_step(); step.has_value(); step = gen.next_step())
///         gen.report(machine.execute_operation(gen.state(), step.value()));
///
/// A step is an operation the runner rolled, or an invariant that operation triggered; the generator orders them.
/// report() must follow each next_step() exactly once, with the outcome of running that step against state().
/// It stops at the first failing step, when no operation owes executions any more, or at a total-operation cap.
struct generator
{
    generator(fuzz_machine const& machine, int seed);

    // The runner holds references into this object.
    generator(generator const&) = delete;
    generator& operator=(generator const&) = delete;

    [[nodiscard]] cc::optional<executed_operation> next_step();
    void report(fuzz_machine::execute_result r);

    /// The state every step runs against.
    [[nodiscard]] fuzz_machine::state& state() { return _state; }

    [[nodiscard]] bool has_failed() const { return _failed; }
    [[nodiscard]] int executed_operations() const { return _executed; }

    /// The recorded program, up to and including the failing step; moved out.
    [[nodiscard]] fuzz_run take_run() { return cc::move(_run); }
    /// The failing step's message; moved out.
    [[nodiscard]] cc::string take_error() { return cc::move(_error); }

private:
    // Protects against setups whose required-execution counts never settle.
    static constexpr int max_operations = 100000;

    fuzz_machine const* _machine = nullptr;
    cc::random _rng;
    fuzz_runner _runner;
    fuzz_machine::state _state;

    fuzz_run _run;
    int _executed = 0;
    bool _failed = false;
    bool _done = false;
    cc::string _error;

    cc::optional<executed_operation> _current; // the step handed out and not yet reported
    bool _current_is_invariant = false;
    cc::vector<executed_operation> _invariants; // the invariants the last operation triggered
    int _next_invariant = 0;                    // index into _invariants
};
} // namespace nx::fuzz::impl
