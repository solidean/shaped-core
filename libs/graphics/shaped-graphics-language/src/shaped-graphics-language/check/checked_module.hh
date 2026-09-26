#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/flat.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/source/diagnostic.hh>

/// A second place a diagnostic points at, with what to read there: the declaration a use conflicts with, a way to fix it.
struct sgl::check::related_note
{
    /// A position in the files `check` was given, as `located_diagnostic::file` is.
    i32 file = 0;
    source_span where;
    cc::string message;

    bool operator==(related_note const&) const = default;
};

/// A diagnostic of the check pass.
/// A module has several files, so the span alone does not say where it points.
struct sgl::check::located_diagnostic
{
    diagnostic what;
    /// The file `what.where` points into, as a position in the files `check` was given.
    i32 file = 0;
    /// What the kind alone cannot say: the construct that is unsupported, the name that is unknown, the loop of a cycle.
    /// For a reader; the kind and the span are what a tool keys on.
    cc::string detail;
    /// In the order they should be read, each printed as a `note:` line after the diagnostic.
    cc::vector<related_note> notes;

    [[nodiscard]] bool operator==(located_diagnostic const& rhs) const
    {
        return what == rhs.what && file == rhs.file && detail == rhs.detail && ast::impl::is_equal(notes, rhs.notes);
    }
};

/// What an `@expect` argument says a test does.
enum class sgl::check::expectation_kind : sgl::u8
{
    /// `.fail`: the run fails, by a check or an assert.
    fail,
    /// `.assert`: the run stops at a false `assert`.
    assert_,
    /// `error = "kind"`: a diagnostic of that kind, a normal or a fatal error, stands in the test.
    error,
    /// `warning = "kind"`: the same, of a warning.
    warning,
};

struct sgl::check::test_expectation
{
    expectation_kind kind = expectation_kind::fail;
    /// The kind name a diagnostic must have, where `*` stands for any run of characters: `type-*`.
    cc::string pattern;
    /// The argument, which an unmet expectation is reported at.
    source_span where;

    bool operator==(test_expectation const&) const = default;
};

/// One `test` of the module, wherever it stands.
struct sgl::check::test_info
{
    symbol_id symbol = symbol_id::none;
    i32 file = 0;
    ast::decl_id declaration = ast::decl_id::none;
    /// The `test` keyword, which is where the test is named in a report.
    source_span where;
    /// From the keyword to the end of the body: a diagnostic inside it is the test's, which `@expect` may name.
    source_span extent;
    /// What stands around it, for a reader: empty at file scope, `struct light`, `fun shade`.
    cc::string scope_path;
    /// The text of a `//` comment on the `test` line or on the line above it, which says what the test is for.
    cc::string comment;
    /// A position in `checked_module::test_units`, -1 for a test with no flat tree.
    i32 unit = -1;
    /// Its `@expect` arguments, in the order written.
    cc::vector<test_expectation> expectations;

    /// True where an expectation names a diagnostic, which makes the test one that is judged by them and never run.
    [[nodiscard]] bool expects_diagnostics() const
    {
        for (auto const& e : expectations)
            if (e.kind == expectation_kind::error || e.kind == expectation_kind::warning)
                return true;
        return false;
    }

    [[nodiscard]] bool operator==(test_info const& rhs) const
    {
        return symbol == rhs.symbol && file == rhs.file && declaration == rhs.declaration && where == rhs.where
            && extent == rhs.extent && scope_path == rhs.scope_path && comment == rhs.comment && unit == rhs.unit
            && ast::impl::is_equal(expectations, rhs.expectations);
    }
};

/// One module after name resolution, type checking and evaluation, as one value beside the ASTs it was checked from.
///
/// It has two readers.
/// An editor reads `files`, the side tables over the untouched AST, and `symbols`.
/// An emitter reads `entry_points` with `types`, `members` and `bindings`, and never the AST.
struct sgl::check::checked_module
{
    /// The module-level declarations of every file, in file order and then in source order.
    cc::vector<symbol> symbols;
    /// Canonical and deduplicated; `types[0]` is the error type and `types[1]` is `void`.
    cc::vector<type_info> types;
    /// The fields of every struct and the members of every binding.
    cc::vector<member_info> members;
    /// The cases of every enum.
    cc::vector<enum_case_info> enum_cases;
    cc::vector<function_info> functions;
    cc::vector<parameter> parameters;
    cc::vector<binding_info> bindings;
    /// The binding lists of the functions, and the layouts of the pipelines.
    cc::vector<symbol_id> binding_lists;
    /// The static samplers a binding declares, which its members name by `member_info::static_sampler`.
    cc::vector<sampler_state> samplers;
    /// Only the pipelines that checked without an error.
    cc::vector<pipeline_info> pipelines;
    cc::vector<pipeline_setting> pipeline_settings;
    /// The value of every `const` that checked.
    cc::vector<constant_info> constants;
    /// How each resolved call fills its callee's parameters; `file_tables::call_of` points in here.
    cc::vector<call_record> call_records;
    cc::vector<written_argument> written_arguments;
    cc::vector<i32> call_slots;
    /// Every candidate of every call that reported `no-matching-overload`, with the reason it did not match.
    cc::vector<near_miss> near_misses;

    /// One entry per file `check` was given, in that order.
    cc::vector<file_tables> files;

    /// Only the entry points that checked without an error; a broken one has diagnostics and no flat tree.
    cc::vector<flat_entry_point> entry_points;

    /// Every test, in the order the pass found them: file scope and type bodies first, then the tests of function bodies.
    cc::vector<test_info> tests;
    /// The flat trees of the tests that checked without an error, each of stage `none` and without a parameter.
    cc::vector<flat_entry_point> test_units;

    /// In the order the demand-driven pass found them, which is deterministic and not source order.
    cc::vector<located_diagnostic> diagnostics;

    /// The registry every `builtin_id` and `builtin_type_id` in here is a position in.
    /// It must outlive the module; `builtins::default_registry()` lives as long as the process.
    builtins::registry const* builtins = nullptr;

    static constexpr type_id error_type = type_id(0);
    /// `types[1]`: what a function without a return type returns.
    static constexpr type_id void_type = type_id(1);

    [[nodiscard]] symbol const& at(symbol_id id) const { return symbols[index_of(id)]; }
    [[nodiscard]] type_info const& at(type_id id) const { return types[index_of(id)]; }
    [[nodiscard]] cc::span<member_info const> at(ast::range_of<member_info> r) const
    {
        return ast::impl::slice(members, r);
    }
    [[nodiscard]] cc::span<enum_case_info const> at(ast::range_of<enum_case_info> r) const
    {
        return ast::impl::slice(enum_cases, r);
    }
    [[nodiscard]] cc::span<parameter const> at(ast::range_of<parameter> r) const
    {
        return ast::impl::slice(parameters, r);
    }
    [[nodiscard]] cc::span<symbol_id const> at(ast::range_of<symbol_id> r) const
    {
        return ast::impl::slice(binding_lists, r);
    }
    [[nodiscard]] cc::span<pipeline_setting const> at(ast::range_of<pipeline_setting> r) const
    {
        return ast::impl::slice(pipeline_settings, r);
    }
    [[nodiscard]] cc::span<written_argument const> at(ast::range_of<written_argument> r) const
    {
        return ast::impl::slice(written_arguments, r);
    }
    [[nodiscard]] cc::span<i32 const> at(ast::range_of<i32> r) const { return ast::impl::slice(call_slots, r); }

    /// The registry record behind a type the prelude declares `@builtin`; null for every other type and for an id that names none.
    [[nodiscard]] builtins::type_record const* builtin_type_of(type_id id) const
    {
        if (builtins == nullptr || !is_valid(id) || index_of(id) >= types.size())
            return nullptr;
        auto const& t = at(id);
        auto const is_declared = t.kind == type_kind::structure || t.kind == type_kind::enumeration;
        if (!is_declared || !is_valid(t.symbol) || index_of(t.symbol) >= symbols.size())
            return nullptr;
        auto const record = at(t.symbol).intrinsic_type;
        return builtins->is_known(record) ? &builtins->at(record) : nullptr;
    }

    /// An enum of the program or the prelude that is no `@builtin`: one whose values are the `int`s of its cases.
    /// `bool` is a builtin enum, so it is none, and it compares and is written as a bool.
    [[nodiscard]] bool is_plain_enum(type_id id) const
    {
        return is_valid(id) && index_of(id) < types.size() && at(id).kind == type_kind::enumeration
            && builtin_type_of(id) == nullptr;
    }

    /// The registry record behind a call; null when `id` names none.
    [[nodiscard]] builtins::function_record const* builtin_function(builtin_id id) const
    {
        return builtins != nullptr && builtins->is_known(id) ? &builtins->at(id) : nullptr;
    }

    /// The name a type is written with; `<error>` for the error type.
    [[nodiscard]] cc::string_view name_of(type_id id) const
    {
        auto const& t = at(id);
        if (t.kind == type_kind::void_)
            return "void";
        if (t.kind == type_kind::structure || t.kind == type_kind::enumeration)
            return at(t.symbol).name;
        if (!t.spelled.empty())
            return t.spelled;
        return "<error>";
    }

    [[nodiscard]] bool operator==(checked_module const& rhs) const
    {
        using ast::impl::is_equal;
        return is_equal(symbols, rhs.symbols) && is_equal(types, rhs.types) && is_equal(members, rhs.members)
            && is_equal(enum_cases, rhs.enum_cases) && is_equal(functions, rhs.functions)
            && is_equal(parameters, rhs.parameters) && is_equal(bindings, rhs.bindings)
            && is_equal(binding_lists, rhs.binding_lists) && is_equal(samplers, rhs.samplers)
            && is_equal(pipelines, rhs.pipelines) && is_equal(pipeline_settings, rhs.pipeline_settings)
            && is_equal(constants, rhs.constants) && is_equal(call_records, rhs.call_records)
            && is_equal(written_arguments, rhs.written_arguments) && is_equal(call_slots, rhs.call_slots)
            && is_equal(near_misses, rhs.near_misses) && is_equal(files, rhs.files)
            && is_equal(entry_points, rhs.entry_points) && is_equal(tests, rhs.tests)
            && is_equal(test_units, rhs.test_units) && is_equal(diagnostics, rhs.diagnostics)
            && builtins == rhs.builtins;
    }
};
