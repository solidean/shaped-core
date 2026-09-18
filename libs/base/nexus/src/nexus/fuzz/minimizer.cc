#include "minimizer.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/math/random.hh>
#include <nexus/fuzz/machine.hh>

namespace nx::fuzz::impl
{
namespace
{
// Keep only steps that can transitively influence the last (failing) step.
cc::vector<logical_step> tree_shake(cc::vector<logical_step> const& steps)
{
    int const n = int(steps.size());
    if (n == 0)
        return steps;

    cc::vector<bool> keep;
    for (int i = 0; i < n; ++i)
        keep.push_back(false);
    keep[n - 1] = true;

    for (int i = n - 1; i >= 0; --i)
        if (keep[i])
            for (int p : steps[i].arg_producers)
                if (p != random_producer)
                    keep[p] = true;

    cc::vector<int> remap;
    for (int i = 0; i < n; ++i)
        remap.push_back(-1);
    cc::vector<logical_step> out;
    for (int i = 0; i < n; ++i)
        if (keep[i])
        {
            remap[i] = int(out.size());
            logical_step ls = steps[i];
            for (int& p : ls.arg_producers)
                if (p != random_producer)
                    p = remap[p];
            out.push_back(cc::move(ls));
        }
    return out;
}

bool is_removable(cc::vector<logical_step> const& steps, int j)
{
    for (auto const& s : steps)
        for (int p : s.arg_producers)
            if (p == j)
                return false;
    return true;
}

cc::vector<logical_step> remove_step(cc::vector<logical_step> const& steps, int j)
{
    cc::vector<logical_step> out;
    for (int i = 0; i < int(steps.size()); ++i)
    {
        if (i == j)
            continue;
        logical_step ls = steps[i];
        for (int& p : ls.arg_producers)
            if (p != random_producer && p > j)
                --p;
        out.push_back(cc::move(ls));
    }
    return out;
}
} // namespace

cc::vector<logical_step> derive_logical(fuzz_run const& run)
{
    auto const& m = *run.machine;

    cc::vector<cc::vector<int>> occupant; // per type: slot -> producing step index
    for (int t = 0; t < m.num_types(); ++t)
        occupant.push_back(cc::vector<int>());

    cc::vector<logical_step> steps;
    for (int i = 0; i < int(run.operations.size()); ++i)
    {
        auto const& op = run.operations[i];
        logical_step ls;
        ls.op = op.operation;
        ls.state = op.state;
        ls.result_must_be_true = op.result_must_be_true;

        for (auto slot : op.arg_slots)
        {
            if (m.is_random_type(slot.type))
                ls.arg_producers.push_back(random_producer);
            else
                ls.arg_producers.push_back(occupant[int(slot.type)][int(slot.value)]);
        }

        if (op.return_slot.type != type_index::invalid)
        {
            auto& occ = occupant[int(op.return_slot.type)];
            if (int(op.return_slot.value) == int(occ.size()))
                occ.push_back(i);
            else
                occ[int(op.return_slot.value)] = i;
        }

        steps.push_back(cc::move(ls));
    }
    return steps;
}

fuzz_run regenerate(fuzz_machine const& m, cc::vector<logical_step> const& steps)
{
    cc::vector<int> count; // per type: number of slots so far
    for (int t = 0; t < m.num_types(); ++t)
        count.push_back(0);

    cc::vector<typed_value_index> step_slot;
    for (int i = 0; i < int(steps.size()); ++i)
        step_slot.push_back(typed_value_index{});

    fuzz_run out;
    out.machine = &m;
    for (int i = 0; i < int(steps.size()); ++i)
    {
        auto const& ls = steps[i];
        executed_operation e;
        e.operation = ls.op;
        e.state = ls.state;
        e.result_must_be_true = ls.result_must_be_true;
        for (int p : ls.arg_producers)
        {
            if (p == random_producer)
                e.arg_slots.push_back(typed_value_index{m.random_type(), value_index(0)});
            else
                e.arg_slots.push_back(step_slot[p]);
        }

        auto const& oi = m.op(ls.op);
        if (oi.return_type != type_index::invalid && !oi.is_invariant)
        {
            int const slot = count[int(oi.return_type)]++;
            e.return_slot = typed_value_index{oi.return_type, value_index(slot)};
            step_slot[i] = e.return_slot;
        }
        else
        {
            e.return_slot = typed_value_index{type_index::invalid, value_index::invalid};
        }

        out.operations.push_back(cc::move(e));
    }
    return out;
}

minimizer::minimizer(fuzz_run const& failing, cc::random& rng)
  : _machine(failing.machine), _rng(&rng), _current(derive_logical(failing))
{
}

cc::optional<fuzz_run> minimizer::next_candidate()
{
    CC_ASSERT(!_awaiting_report, "report() the previous candidate's replay before asking for the next one");

    if (_phase == phase::round_start)
    {
        // Tree-shake first: it usually removes the bulk in one step.
        auto shaken = tree_shake(_current);
        if (shaken.size() < _current.size())
        {
            _candidate = cc::move(shaken);
            _awaiting_report = true;
            return regenerate(*_machine, _candidate);
        }
        begin_removals();
    }

    if (_phase == phase::removals)
    {
        while (_next_removal < int(_order.size()))
        {
            auto const j = _order[_next_removal++];
            if (j == int(_current.size()) - 1) // removing the failing step never helps
                continue;
            if (!is_removable(_current, j))
                continue;

            _candidate = remove_step(_current, j);
            _awaiting_report = true;
            return regenerate(*_machine, _candidate);
        }
        _phase = phase::done; // a whole round without an improvement: a local minimum
    }

    return {};
}

void minimizer::report(fuzz_run::replay_result const& r)
{
    CC_ASSERT(_awaiting_report, "report() needs a candidate from next_candidate()");
    _awaiting_report = false;

    if (r.is_failing())
    {
        adopt(cc::move(_candidate), r.failing_op);
        _phase = phase::round_start;
        return;
    }

    if (_phase == phase::round_start)
    {
        // The shaken program no longer fails, so this round continues with single removals.
        begin_removals();
    }
}

fuzz_run minimizer::result() const
{
    return regenerate(*_machine, _current);
}

void minimizer::begin_removals()
{
    _phase = phase::removals;
    _order.clear();
    for (int i = 0; i < int(_current.size()); ++i)
        _order.push_back(i);
    _rng->shuffle(_order);
    _next_removal = 0;
}

void minimizer::adopt(cc::vector<logical_step> candidate, int failing_op)
{
    while (int(candidate.size()) > failing_op + 1) // drop steps recorded after the failure
        candidate.remove_back();
    _current = cc::move(candidate);
}
} // namespace nx::fuzz::impl
