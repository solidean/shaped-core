#include "run.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/platform/native.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/fuzz/machine.hh>

namespace nx::fuzz
{
namespace
{
// A run, re-expressed in producer/SSA form for analysis and minimization. Each step references the
// step that produced each of its inputs (or `random_producer` for a synthesized cc::random&), so
// the representation is stable under deletion: we delete steps and regenerate fresh absolute slot
// indices, instead of trying to patch absolute indices in place.
constexpr int random_producer = -1;

struct logical_step
{
    op_index op = invalid_index;
    int seed = 0;
    bool result_must_be_true = false;
    cc::vector<int> arg_producers; // random_producer or an index into the step list
};

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
        ls.seed = op.seed;
        ls.result_must_be_true = op.result_must_be_true;

        for (auto slot : op.arg_slots)
        {
            if (m.is_random_type(slot.type))
                ls.arg_producers.push_back(random_producer);
            else
                ls.arg_producers.push_back(occupant[slot.type][slot.value]);
        }

        if (op.return_slot.type != invalid_index)
        {
            auto& occ = occupant[op.return_slot.type];
            if (op.return_slot.value == int(occ.size()))
                occ.push_back(i);
            else
                occ[op.return_slot.value] = i;
        }

        steps.push_back(cc::move(ls));
    }
    return steps;
}

// Regenerate an absolute run from logical steps, assigning every produced value a fresh appended
// slot (so the program is always well-formed as long as each kept step's producers are also kept).
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
        e.seed = ls.seed;
        e.result_must_be_true = ls.result_must_be_true;

        for (int p : ls.arg_producers)
        {
            if (p == random_producer)
                e.arg_slots.push_back(typed_value_index{m.random_type(), 0});
            else
                e.arg_slots.push_back(step_slot[p]);
        }

        auto const& oi = m.op(ls.op);
        if (oi.return_type != invalid_index && !oi.is_invariant)
        {
            int const slot = count[oi.return_type]++;
            e.return_slot = typed_value_index{oi.return_type, slot};
            step_slot[i] = e.return_slot;
        }
        else
        {
            e.return_slot = typed_value_index{invalid_index, invalid_index};
        }

        out.operations.push_back(cc::move(e));
    }
    return out;
}

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

// Prefix every non-empty line of `text` with four spaces (for nesting inside a SECTION block).
cc::string indent_block(cc::string const& text)
{
    cc::string out;
    bool at_line_start = true;
    for (int i = 0; i < int(text.size()); ++i)
    {
        char const c = text[i];
        if (at_line_start && c != '\n')
            out += "    ";
        out += c;
        at_line_start = (c == '\n');
    }
    return out;
}

char name_prefix(std::type_index t)
{
    cc::string const d = cc::demangle_symbol(cc::string_view(t.name()));
    for (int i = 0; i < int(d.size()); ++i)
    {
        char const c = d[i];
        if (cc::is_lower(c) || cc::is_upper(c))
            return cc::to_lower(c);
    }
    return 'v';
}
} // namespace

fuzz_run::replay_result fuzz_run::replay() const
{
    auto const& m = *machine;
    auto s = m.make_initial_state();
    for (int i = 0; i < int(operations.size()); ++i)
    {
        if (!m.preconditions_fulfilled(s, operations[i]))
            return replay_result{.invalid_precondition = true};
        if (!m.execute_operation(s, operations[i]).is_ok())
            return replay_result{.failing_op = i};
    }
    return replay_result{};
}

fuzz_run fuzz_run::minimize(cc::random& rng) const
{
    auto const& m = *machine;
    auto current = derive_logical(*this);

    auto adopt = [&](cc::vector<logical_step> cand, int failing_op)
    {
        while (int(cand.size()) > failing_op + 1) // drop steps recorded after the failure
            cand.remove_back();
        current = cc::move(cand);
    };

    bool improved = true;
    while (improved)
    {
        improved = false;

        // tree-shake first: usually removes the bulk in one step
        auto shaken = tree_shake(current);
        if (shaken.size() < current.size())
        {
            auto r = regenerate(m, shaken).replay();
            if (r.is_failing())
            {
                adopt(cc::move(shaken), r.failing_op);
                improved = true;
                continue;
            }
        }

        // shuffled single-step removals
        cc::vector<int> order;
        for (int i = 0; i < int(current.size()); ++i)
            order.push_back(i);
        rng.shuffle(order);

        for (int j : order)
        {
            if (j == int(current.size()) - 1) // removing the failing step never helps
                continue;
            if (!is_removable(current, j))
                continue;

            auto cand = remove_step(current, j);
            auto r = regenerate(m, cand).replay();
            if (r.is_failing())
            {
                adopt(cc::move(cand), r.failing_op);
                improved = true;
                break;
            }
        }
    }

    return regenerate(m, current);
}

cc::string fuzz_run::emit_regression(cc::string_view test_var, regression_dialect const& dialect) const
{
    auto const& m = *machine;
    auto steps = derive_logical(*this);

    // assign a stable variable name to every producing step
    cc::vector<cc::string> names;
    bool seeded = false;
    for (int i = 0; i < int(steps.size()); ++i)
    {
        names.push_back(cc::string());
        auto const& oi = m.op(steps[i].op);
        if (oi.return_type != invalid_index && !oi.is_invariant)
        {
            cc::string n;
            n += name_prefix(m.type(oi.return_type).std_type);
            n += cc::to_string(i);
            names[i] = cc::move(n);
        }
        for (int p : steps[i].arg_producers)
            if (p == random_producer)
                seeded = true;
    }

    cc::string const var = cc::string::create_copy_of(test_var);

    auto arg_list = [&](logical_step const& ls)
    {
        cc::string out;
        bool first = true;
        for (int p : ls.arg_producers)
        {
            if (!first)
                out += ", ";
            first = false;
            if (p == random_producer)
            {
                out += "random.seeded(";
                out += cc::to_string(ls.seed);
                out += ")";
            }
            else
            {
                out += names[p];
            }
        }
        return out;
    };

    cc::string code;
    if (seeded)
        code += "nx::fuzz::replay_random random;\n";

    for (int i = 0; i < int(steps.size()); ++i)
    {
        auto const& ls = steps[i];
        auto const& oi = m.op(ls.op);
        cc::string const opname = oi.op->name();
        cc::string const args = arg_list(ls);

        bool const is_last = (i == int(steps.size()) - 1);

        if (is_last && ls.result_must_be_true)
        {
            // the failing invariant returned false -> assert the negation reproduces
            code += dialect.assert_macro;
            code += "(!";
            code += var;
            code += "->eval_op_bool(\"";
            code += opname;
            code += "\"";
            if (!args.empty())
            {
                code += ", ";
                code += args;
            }
            code += "));\n";
            continue;
        }

        if (oi.return_type != invalid_index && !oi.is_invariant)
        {
            code += "auto ";
            code += names[i];
            code += " = ";
        }
        code += var;
        code += "->eval_op(\"";
        code += opname;
        code += "\"";
        if (!args.empty())
        {
            code += ", ";
            code += args;
        }
        code += ");";
        if (is_last)
            code += " // <-- fails here";
        code += "\n";
    }

    // wrap the replay in a SECTION so it can sit next to the SECTION that runs the fuzzer
    cc::string out = dialect.section_open;
    out += "\n";
    out += indent_block(code);
    out += dialect.section_close;
    out += "\n";
    return out;
}
} // namespace nx::fuzz
