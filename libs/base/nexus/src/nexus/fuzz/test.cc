#include "test.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <nexus/async-test.hh> // nx::impl::invoking_home_if_any
#include <nexus/fuzz/generator.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/tests/check.hh>
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

// Prints a finding and its reproducer; shared by both drivers.
void report_finding(int seed,
                    test::fuzz_result const& res,
                    fuzz_run const& minimized,
                    cc::string_view test_var,
                    regression_dialect const& dialect)
{
    auto const code = minimized.emit_regression(test_var, dialect);
    cc::eprintln("\n[fuzz] found a failing run (seed {}, {} operations): {}", seed, res.executed_operations,
                 as_sv(res.error_message));
    cc::eprintln("[fuzz] minimal reproducer ({} operations) - paste as a SECTION next to your fuzz SECTION:\n",
                 int(minimized.operations.size()));
    cc::eprintln(as_sv(code));
}

// A finished generator's outcome, as both drivers report it.
test::fuzz_result to_fuzz_result(impl::generator& gen)
{
    if (!gen.has_failed())
        return test::fuzz_result{.is_ok = true, .executed_operations = gen.executed_operations()};
    return test::fuzz_result{.is_ok = false,
                             .executed_operations = gen.executed_operations(),
                             .failing_run = gen.take_run(),
                             .error_message = gen.take_error()};
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

// The wrong eval spelling is a mistake in pasted or hand-written test code, so it fails the test rather than asserting, and holds on every preset.
fuzz_operation* test::sync_op_or_fail(cc::string_view name) const
{
    auto* const op = op_or_die(name);
    if (op->is_async())
        FAIL(cc::format("'{}' is an async op: co_await test->eval_op_async(...) from an async test", name));
    return op;
}

fuzz_operation* test::async_op_or_fail(cc::string_view name) const
{
    auto* const op = op_or_die(name);
    if (!op->is_async())
        FAIL(cc::format("'{}' is a synchronous op: call test->eval_op(...)", name));
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

    auto gen = impl::generator(*_machine, seed);
    for (auto step = gen.next_step(); step.has_value(); step = gen.next_step())
        gen.report(_machine->execute_operation(gen.state(), step.value()));
    return to_fuzz_result(gen);
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
        auto const minimized = res.failing_run.value().minimize(rng);
        report_finding(seed, res, minimized, test_var, _dialect);
        return false;
    }

    return true;
}

cc::async_scheduler* test::inherited_home() const
{
    return _inherit_home ? nx::impl::invoking_home_if_any() : nullptr;
}

cc::shared_async<test::fuzz_result> test::execute_fuzzer_async(int seed)
{
    auto* const home = inherited_home();
    return impl::place(fuzzer_async(seed, home), home);
}

cc::shared_async<bool> test::execute_fuzz_test_async(cc::string_view test_var)
{
    // Both read here, in the caller's segment: the coroutine is cold and may first run elsewhere, after the caller's view is gone.
    auto* const home = inherited_home();
    return impl::place(fuzz_test_async(cc::string(test_var), home), home);
}

// execute_fuzzer, awaiting async steps; a sync step runs inline exactly as there.
cc::shared_async<test::fuzz_result> test::fuzzer_async(int seed, cc::async_scheduler* home)
{
    if (!_machine)
        build_machine();

    if (!_setup_ok)
        co_return fuzz_result{.is_ok = false, .error_message = _setup_error};

    auto gen = impl::generator(*_machine, seed);
    for (auto step = gen.next_step(); step.has_value(); step = gen.next_step())
    {
        if (_machine->op(step.value().operation).is_async)
            gen.report(co_await cc::async_take(
                impl::place(_machine->execute_operation_async(gen.state(), step.value(), home), home)));
        else
            gen.report(_machine->execute_operation(gen.state(), step.value()));
    }
    co_return to_fuzz_result(gen);
}

// execute_fuzz_test, awaiting each program and each shrinking candidate.
cc::shared_async<bool> test::fuzz_test_async(cc::string test_var, cc::async_scheduler* home)
{
    if (!_machine)
        build_machine();

    if (!_setup_ok)
    {
        cc::eprintln("[fuzz] setup error: {}", as_sv(_setup_error));
        co_return false;
    }

    auto seeds = nx::test_random();
    for (auto attempt = 0; attempt < _seed_count; ++attempt)
    {
        auto const seed = int(seeds.next_u32() & 0x7fffffff);
        auto res = co_await cc::async_take(impl::place(fuzzer_async(seed, home), home));
        if (res.is_ok || !res.failing_run.has_value())
            continue;

        auto rng = cc::random(u64(seed));
        auto const minimized
            = co_await cc::async_take(impl::place(res.failing_run.value().minimize_async(rng, home), home));
        report_finding(seed, res, minimized, test_var, _dialect);
        co_return false;
    }

    co_return true;
}
} // namespace nx::fuzz
