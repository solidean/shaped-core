#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fuzz/machine.hh>
#include <nexus/fuzz/operation.hh>
#include <nexus/fuzz/regression_dialect.hh>
#include <nexus/fuzz/replay_random.hh>
#include <nexus/fuzz/run.hh>
#include <nexus/fuzz/value.hh>

namespace nx::fuzz
{
/// The public façade for an API-sequence fuzz test.
///
/// Declare a bag of operations, seed values, and invariants, then execute_fuzz_test() to generate
/// random type-correct programs, detect a failure, shrink it, and print copy-pasteable regression
/// code. The same object also evaluates operations directly (eval_op...), which is what the emitted
/// regression code calls to replay a failing program.
struct test
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

    /// Registers a seed value, modeled as a nullary operation returning a copy. T must be copyable.
    template <class T>
    fuzz_operation* add_value(cc::string name, T value)
    {
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

    // ---- execution -------------------------------------------------------------------------------

    /// Runs a single deterministic fuzz program for the given seed.
    [[nodiscard]] fuzz_result execute_fuzzer(int seed);

    /// Searches several seeds for a failing program; on the first failure, shrinks it and prints
    /// regression code referring to the handle named `test_var`. Returns true if no failure was found.
    [[nodiscard]] bool execute_fuzz_test(cc::string_view test_var = "test");

    // ---- direct evaluation (used by regression code) ---------------------------------------------

    template <class... Args>
    [[nodiscard]] fuzz_value eval_op(cc::string_view op, Args&&... args) const
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

    cc::vector<cc::unique_ptr<fuzz_operation>> _operations;
    cc::unique_ptr<fuzz_machine> _machine;
    cc::string _setup_error;
    bool _setup_ok = false;
    regression_dialect _dialect;
};
} // namespace nx::fuzz
