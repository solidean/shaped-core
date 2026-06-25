#include "machine.hh"

#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/math/random.hh>
#include <clean-core/platform/native.hh>
#include <nexus/tests/check.hh>

#include <exception> // std::exception

namespace nx::fuzz
{
namespace
{
fuzz_machine::execute_result fail(char const* prefix, char const* detail = nullptr)
{
    cc::string s = prefix;
    if (detail != nullptr && detail[0] != '\0')
        s += detail;
    return fuzz_machine::execute_result{.ok = false, .error = cc::move(s)};
}
} // namespace

fuzz_machine::fuzz_machine(cc::span<fuzz_operation* const> ops)
{
    for (auto* op : ops)
    {
        op_info oi;
        oi.op = op;
        oi.is_invariant = op->is_invariant();
        for (auto at : op->arg_types())
            oi.arg_types.push_back(intern(at));
        for (bool m : op->arg_is_mutable())
            oi.arg_is_mutable.push_back(m);
        oi.return_type = op->returns_void() ? invalid_index : intern(op->return_type());
        _operations.push_back(cc::move(oi));
    }

    _random_type = index_of(std::type_index(typeid(cc::random)));

    for (op_index i = 0; i < int(_operations.size()); ++i)
    {
        auto const& oi = _operations[i];
        if (oi.is_invariant)
        {
            if (!oi.arg_types.empty())
                _types[oi.arg_types[0]].invariant_ops.push_back(i);
        }
        else if (oi.return_type != invalid_index)
        {
            _types[oi.return_type].creating_ops.push_back(i);
        }
    }
}

type_index fuzz_machine::intern(std::type_index t)
{
    for (type_index i = 0; i < int(_types.size()); ++i)
        if (_types[i].std_type == t)
            return i;
    type_info ti;
    ti.std_type = t;
    _types.push_back(cc::move(ti));
    return int(_types.size()) - 1;
}

type_index fuzz_machine::index_of(std::type_index t) const
{
    for (type_index i = 0; i < int(_types.size()); ++i)
        if (_types[i].std_type == t)
            return i;
    return invalid_index;
}

bool fuzz_machine::assert_is_properly_set_up(cc::string& out_error) const
{
    // A type is "satisfied" if it is unneeded or constructible. We seed every type as satisfied,
    // then mark types used as (non-random) arguments unsatisfied, then iterate a creatability
    // fixpoint: a type becomes satisfied once some creating op has all-satisfied argument types.
    cc::vector<bool> satisfied;
    for (type_index i = 0; i < int(_types.size()); ++i)
        satisfied.push_back(true);

    for (auto const& oi : _operations)
        for (type_index at : oi.arg_types)
            if (!is_random_type(at))
                satisfied[at] = false;

    if (_random_type != invalid_index)
        satisfied[_random_type] = true;

    auto all_ok = [&]
    {
        for (bool b : satisfied)
            if (!b)
                return false;
        return true;
    };

    while (!all_ok())
    {
        bool changed = false;
        for (type_index t = 0; t < int(_types.size()); ++t)
        {
            if (satisfied[t])
                continue;
            for (op_index opi : _types[t].creating_ops)
            {
                bool can = true;
                for (type_index at : _operations[opi].arg_types)
                    if (!satisfied[at])
                    {
                        can = false;
                        break;
                    }
                if (can)
                {
                    satisfied[t] = true;
                    changed = true;
                    break;
                }
            }
        }

        if (!changed)
        {
            out_error = "fuzz setup error: the following argument types can never be constructed: ";
            bool first = true;
            for (type_index t = 0; t < int(_types.size()); ++t)
                if (!satisfied[t])
                {
                    if (!first)
                        out_error += ", ";
                    out_error += cc::demangle_symbol(cc::string_view(_types[t].std_type.name()));
                    first = false;
                }
            return false;
        }
    }
    return true;
}

fuzz_machine::state fuzz_machine::make_initial_state() const
{
    state s;
    for (type_index i = 0; i < int(_types.size()); ++i)
        s.values_by_type.push_back(cc::vector<fuzz_value>());
    return s;
}

cc::span<fuzz_value*> fuzz_machine::assemble_args(state& s,
                                                  executed_operation const& exec,
                                                  cc::vector<fuzz_value>& synth,
                                                  cc::vector<fuzz_value*>& buf) const
{
    int rng = 0;
    for (auto slot : exec.arg_slots)
        if (is_random_type(slot.type))
            ++rng;
    synth.reserve(rng); // keep &synth.back() stable below

    for (auto slot : exec.arg_slots)
    {
        if (is_random_type(slot.type))
        {
            synth.push_back(fuzz_value::create(cc::random(u64(exec.seed))));
            buf.push_back(&synth.back());
        }
        else
        {
            buf.push_back(&s.values_by_type[slot.type][slot.value]);
        }
    }
    return cc::span<fuzz_value*>(buf);
}

bool fuzz_machine::preconditions_fulfilled(state const& s, executed_operation const& exec) const
{
    auto const& oi = _operations[exec.operation];
    cc::vector<fuzz_value> synth;
    cc::vector<fuzz_value*> buf;
    // assemble reads slots; the const_cast is safe because precondition evaluation never mutates.
    auto args = assemble_args(const_cast<state&>(s), exec, synth, buf);
    return oi.op->check_preconditions(args);
}

fuzz_machine::execute_result fuzz_machine::execute_operation(state& s, executed_operation const& exec) const
{
    auto const& oi = _operations[exec.operation];

    cc::vector<fuzz_value> synth;
    cc::vector<fuzz_value*> buf;
    auto args = assemble_args(s, exec, synth, buf);

    // Capture CHECK/REQUIRE failures (without aborting or polluting the host test) and reroute a
    // failing CC_ASSERT into an exception (CC_ASSERT would otherwise abort right after the handler).
    nx::impl::check_capture_sink sink;
    nx::impl::scoped_check_capture cap(sink);
    auto handler = cc::impl::scoped_assertion_handler([](cc::impl::assertion_info const& info)
                                                      { throw impl::assertion_failure{info.message}; });

    fuzz_value result;
    try
    {
        result = oi.op->invoke(args);
    }
    catch (impl::assertion_failure const& e)
    {
        return fail("assertion failed: ", e.message.c_str());
    }
    catch (std::exception const& e)
    {
        return fail("uncaught exception: ", e.what());
    }
    catch (...)
    {
        return fail("uncaught unknown exception");
    }

    if (sink.failed > 0)
    {
        if (sink.first_message.empty())
            return fail("a CHECK/REQUIRE failed");
        cc::string msg = "a CHECK/REQUIRE failed: ";
        msg += sink.first_message;
        return fuzz_machine::execute_result{.ok = false, .error = cc::move(msg)};
    }

    if (exec.result_must_be_true && (!result.is_valid() || !result.get_bool()))
        return fail("invariant violated");

    auto const rs = exec.return_slot;
    if (rs.type != invalid_index && result.is_valid())
    {
        auto& slots = s.values_by_type[rs.type];
        if (rs.value == int(slots.size()))
            slots.push_back(cc::move(result));
        else
        {
            CC_ASSERT(rs.value >= 0 && rs.value < int(slots.size()), "return slot out of range");
            slots[rs.value] = cc::move(result);
        }
    }

    return fuzz_machine::execute_result{};
}

cc::vector<executed_operation> fuzz_machine::create_invariant_executions_for(executed_operation const& exec) const
{
    cc::vector<executed_operation> result;

    auto add_for_slot = [&](typed_value_index slot)
    {
        if (slot.type == invalid_index)
            return;
        for (op_index inv : _types[slot.type].invariant_ops)
        {
            executed_operation e;
            e.operation = inv;
            e.arg_slots.push_back(slot);
            e.result_must_be_true = !_operations[inv].op->returns_void();
            result.push_back(cc::move(e));
        }
    };

    add_for_slot(exec.return_slot);

    auto const& oi = _operations[exec.operation];
    for (int i = 0; i < int(exec.arg_slots.size()); ++i)
        if (i < int(oi.arg_is_mutable.size()) && oi.arg_is_mutable[i] && !is_random_type(exec.arg_slots[i].type))
            add_for_slot(exec.arg_slots[i]);

    return result;
}
} // namespace nx::fuzz
