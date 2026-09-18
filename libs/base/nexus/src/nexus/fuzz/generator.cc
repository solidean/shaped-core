#include "generator.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>

namespace nx::fuzz::impl
{
generator::generator(fuzz_machine const& machine, int seed)
  : _machine(&machine), _rng(u64(seed)), _runner(machine, _rng), _state(machine.make_initial_state())
{
    _run.machine = &machine;
}

cc::optional<executed_operation> generator::next_step()
{
    CC_ASSERT(!_current.has_value(), "report() the previous step's outcome before asking for the next one");

    if (_done)
        return {};

    if (_next_invariant < int(_invariants.size()))
    {
        _current = _invariants[_next_invariant++];
        _current_is_invariant = true;
        return _current;
    }

    // Only between operations, so an operation's invariants always run.
    if (_executed >= max_operations || !_runner.should_continue())
    {
        _done = true;
        return {};
    }

    auto exec = executed_operation();
    if (!_runner.create_next_execution(_state, exec))
    {
        _done = true;
        return {};
    }
    _current = cc::move(exec);
    _current_is_invariant = false;
    return _current;
}

void generator::report(fuzz_machine::execute_result r)
{
    CC_ASSERT(_current.has_value(), "report() needs a step from next_step()");

    _run.operations.push_back(_current.value());
    ++_executed;

    if (!r.is_ok())
    {
        _failed = true;
        _done = true;
        _error = cc::move(r.error);
    }
    else if (!_current_is_invariant)
    {
        _invariants = _machine->create_invariant_executions_for(_current.value());
        _next_invariant = 0;
    }
    _current = {};
}
} // namespace nx::fuzz::impl
