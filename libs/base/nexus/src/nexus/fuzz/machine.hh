#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <nexus/fuzz/executed_operation.hh>
#include <nexus/fuzz/fwd.hh>
#include <nexus/fuzz/operation.hh>
#include <nexus/fwd.hh>
#include <nexus/tests/check.hh>
#include <nexus/tests/check_divert.hh>
#include <nexus/tests/typed_value.hh>

#include <typeindex>

/// The runtime model of a fuzz test: types and operations flattened into dense, integer-indexed
/// tables, plus the execution of a single recorded step against a mutable state.
///
/// The machine is built once from the user's operations and is then immutable; all per-run data
/// lives in `state`. Operations communicate only through slots, never through object identity,
/// which is what makes runs reproducible, analyzable, and minimizable.
struct nx::fuzz::fuzz_machine
{
    struct op_info
    {
        fuzz_operation* op = nullptr;
        cc::vector<type_index> arg_types; // interned; a cc::random& argument uses the random type
        cc::vector<bool> arg_is_mutable;
        type_index return_type = type_index::invalid; // invalid for void
        bool is_invariant = false;
        bool is_async = false; // returns a cc::shared_async the engine awaits
    };

    struct type_info
    {
        std::type_index std_type = std::type_index(typeid(void));
        cc::vector<op_index> creating_ops;  // non-invariant ops returning this type
        cc::vector<op_index> invariant_ops; // invariants over this type
    };

    /// Per-run mutable values, grouped by interned type.
    /// Slot growth is append-only within a run.
    struct state
    {
        cc::vector<cc::vector<typed_value>> values_by_type;

        [[nodiscard]] int count_of(type_index t) const { return int(values_by_type[int(t)].size()); }
    };

    struct execute_result
    {
        bool ok = true;
        cc::string error;

        [[nodiscard]] bool is_ok() const { return ok; }
    };

    explicit fuzz_machine(cc::span<fuzz_operation* const> ops);

    /// Verifies every type required as an argument can actually be constructed by some operation
    /// (creatability fixpoint). Returns false and fills `out_error` on a setup error.
    [[nodiscard]] bool assert_is_properly_set_up(cc::string& out_error) const;

    [[nodiscard]] state make_initial_state() const;

    [[nodiscard]] int num_operations() const { return int(_operations.size()); }
    [[nodiscard]] int num_types() const { return int(_types.size()); }
    [[nodiscard]] op_info const& op(op_index i) const { return _operations[int(i)]; }
    [[nodiscard]] type_info const& type(type_index i) const { return _types[int(i)]; }

    [[nodiscard]] type_index random_type() const { return _random_type; }
    [[nodiscard]] bool is_random_type(type_index t) const
    {
        return t == _random_type && _random_type != type_index::invalid;
    }

    /// Looks up the interned index of a runtime type, or type_index::invalid if the machine never saw it.
    [[nodiscard]] type_index index_of(std::type_index t) const;

    /// A step whose op has been called but whose outcome is not judged yet.
    /// It owns everything the op may still point into — the synthesized cc::random arguments and the capture sink — so it must outlive the op's work.
    struct started_step
    {
        typed_value result;
        cc::optional<execute_result> failure; // the op threw or asserted
        nx::impl::check_capture_sink sink;
        cc::vector<typed_value> synth;
    };

    /// Runs one step against the state: start_step then finish_step.
    /// Detects thrown exceptions, captured CHECK/REQUIRE failures, failed CC_ASSERTs and false bool invariants, mapping any of them to a failing result.
    [[nodiscard]] execute_result execute_operation(state& s, executed_operation const& exec) const;

    /// Calls the step's op under the check capture and the rerouted assertion handler.
    [[nodiscard]] started_step start_step(state& s, executed_operation const& exec) const;

    /// Judges a started step and, if it passed, writes its result into the return slot.
    [[nodiscard]] execute_result finish_step(state& s, executed_operation const& exec, started_step& step) const;

    /// Checks the operation's preconditions against the prospective input slots (no mutation).
    [[nodiscard]] bool preconditions_fulfilled(state const& s, executed_operation const& exec) const;

    /// Builds the invariant checks triggered by what `exec` produced or mutated.
    [[nodiscard]] cc::vector<executed_operation> create_invariant_executions_for(executed_operation const& exec) const;

private:
    type_index intern(std::type_index t);
    cc::span<typed_value*> assemble_args(state& s,
                                         executed_operation const& exec,
                                         cc::vector<typed_value>& synth,
                                         cc::vector<typed_value*>& buf) const;

    cc::vector<op_info> _operations;
    cc::vector<type_info> _types;
    type_index _random_type = type_index::invalid;
};
