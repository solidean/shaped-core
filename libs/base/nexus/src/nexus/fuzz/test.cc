#include "test.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/fuzz/runner.hh>
#include <nexus/tests/seed.hh>

#include <string_view> // bridges cc::string into std::ostream

namespace nx::fuzz
{
namespace
{
std::string_view as_sv(cc::string const& s)
{
    return std::string_view(s.data(), size_t(s.size()));
}

// "'a'", "'a' and 'b'", "'a', 'b' and 'c'"
cc::string join_names(cc::span<cc::string const> names)
{
    cc::string out;
    for (auto i = 0; i < int(names.size()); ++i)
    {
        if (i > 0)
            out += i == int(names.size()) - 1 ? " and " : ", ";
        out += names[i];
    }
    return out;
}
} // namespace

test::~test() = default;

fuzz_operation* test::add(cc::unique_ptr<fuzz_operation> op)
{
    CC_ASSERT(!_machine, "operations must be added before the fuzzer runs");
    auto* raw = op.get();
    _operations.push_back(cc::move(op));
    return raw;
}

fuzz_operation* test::get_operation_by_name(cc::string_view name) const
{
    for (auto const& op : _operations)
        if (op->name() == name)
            return op.get();
    return nullptr;
}

fuzz_operation* test::op_or_die(cc::string_view name) const
{
    auto* op = get_operation_by_name(name);
    CC_ASSERT(op != nullptr, "no fuzz operation with the requested name");
    return op;
}

void test::build_machine()
{
    cc::vector<fuzz_operation*> raw;
    for (auto const& op : _operations)
        raw.push_back(op.get());
    _machine = cc::make_unique<fuzz_machine>(cc::span<fuzz_operation* const>(raw));
    _setup_ok = _machine->assert_is_properly_set_up(_setup_error);

    // The synchronous drivers cannot await, so a machine holding an async op is theirs to refuse — as a setup error, so it holds on every preset.
    cc::vector<cc::string> async_names;
    for (auto const& op : _operations)
        if (op->is_async())
            async_names.push_back(cc::format("'{}'", op->name()));
    if (!async_names.empty())
        _async_ops_error = cc::format("{} {} async; co_await execute_fuzz_test_async() from an async test instead",
                                      join_names(async_names), async_names.size() == 1 ? "is an" : "are");
}

void test::cap_max_executions(int times)
{
    CC_ASSERT(times >= 0, "an execution cap must be >= 0");
    for (auto const& op : _operations)
        if (op->execute_at_most_times() > times)
            op->execute_at_most(times);
}

void test::cap_seed_count(int count)
{
    CC_ASSERT(count >= 1, "a fuzz needs at least one seed");
    _seed_count = cc::min(_seed_count, count);
}

test::fuzz_result test::execute_fuzzer(int seed)
{
    if (!_machine)
        build_machine();

    if (!_setup_ok || !_async_ops_error.empty())
    {
        fuzz_result r;
        r.is_ok = false;
        r.error_message = !_setup_ok ? _setup_error : _async_ops_error;
        return r;
    }

    // total-operation guard: protects against setups whose required-execution counts never settle.
    constexpr int max_operations = 100000;

    auto rng = cc::random(u64(seed));
    fuzz_runner runner(*_machine, rng);
    auto state = _machine->make_initial_state();

    fuzz_run run;
    run.machine = _machine.get();
    int executed = 0;

    auto failure = [&](cc::string error) -> fuzz_result
    {
        fuzz_result r;
        r.is_ok = false;
        r.executed_operations = executed;
        r.error_message = cc::move(error);
        r.failing_run = cc::move(run);
        return r;
    };

    while (runner.should_continue())
    {
        executed_operation exec;
        if (!runner.create_next_execution(state, exec))
            break;

        auto res = _machine->execute_operation(state, exec);
        run.operations.push_back(exec);
        ++executed;
        if (!res.is_ok())
            return failure(cc::move(res.error));

        // invariants triggered by what this operation produced or mutated
        for (auto const& inv : _machine->create_invariant_executions_for(exec))
        {
            auto ir = _machine->execute_operation(state, inv);
            run.operations.push_back(inv);
            ++executed;
            if (!ir.is_ok())
                return failure(cc::move(ir.error));
        }

        if (executed >= max_operations)
            break;
    }

    fuzz_result ok;
    ok.is_ok = true;
    ok.executed_operations = executed;
    return ok;
}

bool test::execute_fuzz_test(cc::string_view test_var)
{
    if (!_machine)
        build_machine();

    if (!_setup_ok)
    {
        cc::eprintln("[fuzz] setup error: {}", as_sv(_setup_error));
        return false;
    }
    if (!_async_ops_error.empty())
    {
        cc::eprintln("[fuzz] setup error: {}", as_sv(_async_ops_error));
        return false;
    }

    // Drawn from the test's seed, so every run explores different programs and a failure's seed replays under --seed.
    auto seeds = nx::test_random();
    for (auto attempt = 0; attempt < _seed_count; ++attempt)
    {
        auto const seed = int(seeds.next_u32() & 0x7fffffff);
        auto res = execute_fuzzer(seed);
        if (res.is_ok || !res.failing_run.has_value())
            continue;

        auto rng = cc::random(u64(seed));
        auto minimized = res.failing_run.value().minimize(rng);
        auto code = minimized.emit_regression(test_var, _dialect);

        cc::eprintln("\n[fuzz] found a failing run (seed {}, {} operations): {}", seed, res.executed_operations,
                     as_sv(res.error_message));
        cc::eprintln("[fuzz] minimal reproducer ({} operations) - paste as a SECTION next to your fuzz SECTION:\n",
                     int(minimized.operations.size()));
        cc::eprintln(as_sv(code));
        return false;
    }

    return true;
}
} // namespace nx::fuzz
