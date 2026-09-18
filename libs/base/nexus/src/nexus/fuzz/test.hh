#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fuzz/fwd.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/fuzz/operation.hh>
#include <nexus/fuzz/regression_dialect.hh>
#include <nexus/fuzz/run.hh>
#include <nexus/fwd.hh>
#include <nexus/tests/typed_value.hh>

/// The public façade for an API-sequence fuzz test.
///
/// Declare a bag of operations, seed values and invariants, then call execute_fuzz_test().
/// It generates random type-correct programs, detects a failure, shrinks it, and prints copy-pasteable regression code.
/// The same object also evaluates operations directly (eval_op...), which is what the emitted regression code calls to replay a failing program.
///
/// libs/base/nexus/docs/fuzz-testing.md is the full mechanism, including the shared-state trap.
struct nx::fuzz::test
{
    struct fuzz_result
    {
        bool is_ok = true;
        int executed_operations = 0;
        cc::optional<fuzz_run> failing_run; // present on a real finding (absent on setup errors)
        cc::string error_message;
    };

    [[nodiscard]] static cc::unique_ptr<test> create(regression_dialect dialect = nexus_section_dialect())
    {
        auto t = cc::make_unique<test>();
        t->_dialect = cc::move(dialect);
        return t;
    }

    // ---- setup -----------------------------------------------------------------------------------

    /// Registers an operation (a mutating/producing call). Returns the operation for chaining.
    template <class F>
    fuzz_operation* add_op(cc::string name, F&& fn)
    {
        return add(fuzz_operation::create(cc::move(name), cc::forward<F>(fn)));
    }

    /// Registers a seed value, modeled as a nullary operation returning a copy.
    /// T must be copyable.
    template <class T>
    fuzz_operation* add_value(cc::string name, T value)
    {
        static_assert(!impl::async_result_of<T>::is_async && !impl::async_result_of<T>::is_scheduled,
                      "a seed value is a constant, so it cannot be a cc::shared_async — to keep a handle in a slot, "
                      "wrap "
                      "it in a type of your own");
        auto* op = add(fuzz_operation::create(cc::move(name), [value = cc::move(value)]() { return value; }));
        op->execute_at_least(1);
        return op;
    }

    /// Registers an invariant: a univariate, non-mutating check (bool-returning or void+CHECK) run
    /// automatically after any operation that produces or mutates a value of its argument type.
    template <class F>
    fuzz_operation* add_invariant(cc::string name, F&& fn)
    {
        auto* op = add(fuzz_operation::create(cc::move(name), cc::forward<F>(fn)));
        op->mark_as_invariant();
        return op;
    }

    [[nodiscard]] fuzz_operation* get_operation_by_name(cc::string_view name) const;

    // ---- narrowing ---------------------------------------------------------------------------------

    /// Lowers every operation's at-most to `times`, pulling an at-least above it down along with it.
    /// Meant for after all operations are declared, as the narrowing a default run applies under `if (!nx::is_thorough())`.
    void cap_max_executions(int times);

    /// Lowers how many seeds execute_fuzz_test searches, 256 by default.
    /// Each seed is a whole program, so this is the knob that scales the fuzz's runtime linearly.
    void cap_seed_count(int count);

    // ---- execution -------------------------------------------------------------------------------

    /// Runs a single deterministic fuzz program for the given seed.
    [[nodiscard]] fuzz_result execute_fuzzer(int seed);

    /// Searches several seeds for a failing program; on the first failure, shrinks it and prints
    /// regression code referring to the handle named `test_var`. Returns true if no failure was found.
    [[nodiscard]] bool execute_fuzz_test(cc::string_view test_var = "test");

    // ---- async execution (ops returning cc::shared_async; see nexus/fuzz/async.hh) ----------------

    /// execute_fuzz_test for a test holding async ops, which the synchronous entry points refuse:
    /// `CHECK(co_await test->execute_fuzz_test_async());` from an async test.
    /// Any mix of sync and async ops works, and steps still run one at a time: each async op is awaited before the next step starts.
    /// While an op is awaited, every check reported for the running test is the step's, from whichever thread reports it.
    /// That includes work an earlier step started and never awaited.
    /// Cold: nothing runs until it is awaited.
    [[nodiscard]] cc::shared_async<bool> execute_fuzz_test_async(cc::string_view test_var = "test");

    /// execute_fuzzer for a test holding async ops.
    [[nodiscard]] cc::shared_async<fuzz_result> execute_fuzzer_async(int seed);

    /// Run the awaited fuzz where its entry point is called from, when that caller runs in a home — a thread-bound device's thread, say.
    /// Every sync op and every async op's body then runs there; a coroutine an op awaits for itself follows cc's usual placement.
    /// A caller in no home is unaffected, so one test body serves homed and unhomed drivers alike.
    void set_inherit_home(bool inherit) { _inherit_home = inherit; }

    // ---- direct evaluation (used by regression code) ---------------------------------------------

    template <class... Args>
    [[nodiscard]] typed_value eval_op(cc::string_view op, Args&&... args) const
    {
        return op_or_die(op)->eval(cc::forward<Args>(args)...);
    }
    template <class T, class... Args>
    [[nodiscard]] T eval_op_to(cc::string_view op, Args&&... args) const
    {
        return op_or_die(op)->template eval_to<T>(cc::forward<Args>(args)...);
    }
    template <class... Args>
    [[nodiscard]] bool eval_op_bool(cc::string_view op, Args&&... args) const
    {
        return op_or_die(op)->eval_bool(cc::forward<Args>(args)...);
    }

    test() = default;
    ~test();
    test(test const&) = delete;
    test& operator=(test const&) = delete;

private:
    fuzz_operation* add(cc::unique_ptr<fuzz_operation> op);
    fuzz_operation* op_or_die(cc::string_view name) const;
    void build_machine();

    // The home set_inherit_home asks for, read in the caller's segment; null when there is none to inherit.
    [[nodiscard]] cc::async_scheduler* inherited_home() const;
    [[nodiscard]] cc::shared_async<fuzz_result> fuzzer_async(int seed, cc::async_scheduler* home);
    [[nodiscard]] cc::shared_async<bool> fuzz_test_async(cc::string test_var, cc::async_scheduler* home);

    cc::vector<cc::unique_ptr<fuzz_operation>> _operations;
    cc::unique_ptr<fuzz_machine> _machine;
    cc::string _setup_error;
    bool _setup_ok = false;
    cc::string _async_ops_error; // set when any op is async, which the synchronous entry points refuse
    regression_dialect _dialect;
    int _seed_count = 256;
    bool _inherit_home = false;
};
