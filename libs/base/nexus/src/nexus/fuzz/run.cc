#include "run.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/platform/native.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/to_string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/fuzz/minimizer.hh>

namespace nx::fuzz
{
namespace
{
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

cc::shared_async<fuzz_run::replay_result> fuzz_run::replay_async(cc::async_scheduler* home) const
{
    auto const& m = *machine;
    auto s = m.make_initial_state();
    for (int i = 0; i < int(operations.size()); ++i)
    {
        auto const& exec = operations[i];
        if (!m.preconditions_fulfilled(s, exec))
            co_return replay_result{.invalid_precondition = true};

        auto ok = true;
        if (m.op(exec.operation).is_async)
            ok = (co_await impl::place(m.execute_operation_async(s, exec, home), home)).is_ok();
        else
            ok = m.execute_operation(s, exec).is_ok();
        if (!ok)
            co_return replay_result{.failing_op = i};
    }
    co_return replay_result{};
}

fuzz_run fuzz_run::minimize(cc::random& rng) const
{
    auto shrink = impl::minimizer(*this, rng);
    for (auto candidate = shrink.next_candidate(); candidate.has_value(); candidate = shrink.next_candidate())
        shrink.report(candidate.value().replay());
    return shrink.result();
}

cc::shared_async<fuzz_run> fuzz_run::minimize_async(cc::random& rng, cc::async_scheduler* home) const
{
    auto shrink = impl::minimizer(*this, rng);
    for (auto candidate = shrink.next_candidate(); candidate.has_value(); candidate = shrink.next_candidate())
        shrink.report(co_await impl::place(candidate.value().replay_async(home), home));
    co_return shrink.result();
}

cc::string fuzz_run::emit_regression(cc::string_view test_var, regression_dialect const& dialect) const
{
    auto const& m = *machine;
    auto steps = impl::derive_logical(*this);

    // assign a stable variable name to every producing step
    cc::vector<cc::string> names;
    for (int i = 0; i < int(steps.size()); ++i)
    {
        names.push_back(cc::string());
        auto const& oi = m.op(steps[i].op);
        if (oi.return_type != type_index::invalid && !oi.is_invariant)
        {
            cc::string n;
            n += name_prefix(m.type(oi.return_type).std_type);
            n += cc::to_string(i);
            names[i] = cc::move(n);
        }
    }

    cc::string const var = cc::string::create_copy_of(test_var);

    auto arg_list = [&](impl::logical_step const& ls)
    {
        cc::string out;
        bool first = true;
        for (int p : ls.arg_producers)
        {
            if (!first)
                out += ", ";
            first = false;
            if (p == impl::random_producer)
            {
                out += "cc::random::from_state(";
                out += cc::to_string(ls.state);
                out += "ull)";
            }
            else
            {
                out += names[p];
            }
        }
        return out;
    };

    cc::string code;

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

        if (oi.return_type != type_index::invalid && !oi.is_invariant)
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
