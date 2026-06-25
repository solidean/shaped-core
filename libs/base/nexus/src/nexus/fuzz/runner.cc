#include "runner.hh"

#include <clean-core/math/random.hh>

namespace nx::fuzz
{
fuzz_runner::fuzz_runner(fuzz_machine const& machine, cc::random& rng) : _machine(machine), _rng(rng)
{
    for (int i = 0; i < _machine.num_operations(); ++i)
        _exec_count.push_back(0);
}

bool fuzz_runner::should_continue() const
{
    for (op_index i = 0; i < _machine.num_operations(); ++i)
        if (_exec_count[i] < _machine.op(i).op->execute_at_least_times())
            return true;
    return false;
}

bool fuzz_runner::eligible(op_index op, fuzz_machine::state const& s) const
{
    auto const& oi = _machine.op(op);
    if (_exec_count[op] >= oi.op->execute_at_most_times())
        return false;
    for (type_index at : oi.arg_types)
        if (!_machine.is_random_type(at) && s.count_of(at) == 0)
            return false;
    return true;
}

bool fuzz_runner::try_roll(op_index op, fuzz_machine::state const& s, executed_operation& out) const
{
    auto const& oi = _machine.op(op);

    out = executed_operation{};
    out.operation = op;
    // The seed is rolled for every operation (even ones that ignore it) so the RNG stream order is
    // stable; operations taking a cc::random& are seeded from it, which is what makes runs replayable.
    out.seed = _rng.uniform(1000, 9999);

    for (type_index at : oi.arg_types)
    {
        if (_machine.is_random_type(at))
        {
            out.arg_slots.push_back(typed_value_index{at, 0});
            continue;
        }
        int const count = s.count_of(at);
        if (count == 0)
            return false;
        out.arg_slots.push_back(typed_value_index{at, _rng.uniform(0, count - 1)});
    }

    if (!_machine.preconditions_fulfilled(s, out))
        return false;

    if (oi.return_type != invalid_index)
    {
        int const count = s.count_of(oi.return_type);
        // [0, count] inclusive: drawing `count` appends a new slot, which is how the state grows.
        out.return_slot = typed_value_index{oi.return_type, _rng.uniform(0, count)};
    }
    else
    {
        out.return_slot = typed_value_index{invalid_index, invalid_index};
    }
    return true;
}

bool fuzz_runner::create_next_execution(fuzz_machine::state const& s, executed_operation& out)
{
    if (_non_progress > max_consecutive_non_progress)
        return false;

    // Phase 1: prefer operations that still owe required executions and are currently runnable.
    cc::vector<op_index> open_eligible;
    for (op_index i = 0; i < _machine.num_operations(); ++i)
        if (_exec_count[i] < _machine.op(i).op->execute_at_least_times() && eligible(i, s))
            open_eligible.push_back(i);

    if (!open_eligible.empty())
    {
        for (int attempt = 0; attempt < max_eligible_recheck; ++attempt)
        {
            op_index op = _rng.uniform_in(open_eligible);
            if (try_roll(op, s, out))
            {
                ++_exec_count[op];
                _non_progress = 0;
                return true;
            }
        }
    }

    // Phase 2: fallback - run any eligible operation to manufacture missing prerequisite values.
    cc::vector<op_index> all_eligible;
    for (op_index i = 0; i < _machine.num_operations(); ++i)
        if (!_machine.op(i).is_invariant && eligible(i, s))
            all_eligible.push_back(i);

    if (!all_eligible.empty())
    {
        for (int attempt = 0; attempt < max_fallback_check; ++attempt)
        {
            op_index op = _rng.uniform_in(all_eligible);
            if (try_roll(op, s, out))
            {
                ++_exec_count[op];
                ++_non_progress;
                return true;
            }
        }
    }

    return false;
}
} // namespace nx::fuzz
