#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/flat.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/source/diagnostic.hh>

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

    bool operator==(located_diagnostic const&) const = default;
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
    /// Canonical and deduplicated; `types[0]` is the error type and `types[1]` the type of no value.
    cc::vector<type_info> types;
    /// The fields of every struct and the members of every binding.
    cc::vector<member_info> members;
    cc::vector<function_info> functions;
    cc::vector<parameter> parameters;
    cc::vector<binding_info> bindings;
    /// The binding lists of the functions.
    cc::vector<symbol_id> binding_lists;

    /// One entry per file `check` was given, in that order.
    cc::vector<file_tables> files;

    /// Only the entry points that checked without an error; a broken one has diagnostics and no flat tree.
    cc::vector<flat_entry_point> entry_points;

    /// In the order the demand-driven pass found them, which is deterministic and not source order.
    cc::vector<located_diagnostic> diagnostics;

    /// The registry every `builtin_id` and `builtin_type_id` in here is a position in.
    /// It must outlive the module; `builtins::default_registry()` lives as long as the process.
    builtins::registry const* builtins = nullptr;

    static constexpr type_id error_type = type_id(0);
    /// `types[1]`: what a function without a return type returns.
    static constexpr type_id nothing_type = type_id(1);

    [[nodiscard]] symbol const& at(symbol_id id) const { return symbols[index_of(id)]; }
    [[nodiscard]] type_info const& at(type_id id) const { return types[index_of(id)]; }
    [[nodiscard]] cc::span<member_info const> at(ast::range_of<member_info> r) const
    {
        return ast::impl::slice(members, r);
    }
    [[nodiscard]] cc::span<parameter const> at(ast::range_of<parameter> r) const
    {
        return ast::impl::slice(parameters, r);
    }
    [[nodiscard]] cc::span<symbol_id const> at(ast::range_of<symbol_id> r) const
    {
        return ast::impl::slice(binding_lists, r);
    }

    /// The registry record behind a type the prelude declares `@builtin`; null for every other type and for an id that names none.
    [[nodiscard]] builtins::type_record const* builtin_type_of(type_id id) const
    {
        if (builtins == nullptr || !is_valid(id) || index_of(id) >= types.size())
            return nullptr;
        auto const& t = at(id);
        if (t.kind != type_kind::structure || !is_valid(t.symbol) || index_of(t.symbol) >= symbols.size())
            return nullptr;
        auto const record = at(t.symbol).intrinsic_type;
        return builtins->is_known(record) ? &builtins->at(record) : nullptr;
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
        if (t.kind == type_kind::nothing)
            return "nothing";
        return t.kind == type_kind::structure ? cc::string_view(at(t.symbol).name) : cc::string_view("<error>");
    }

    [[nodiscard]] bool operator==(checked_module const& rhs) const
    {
        using ast::impl::is_equal;
        return is_equal(symbols, rhs.symbols) && is_equal(types, rhs.types) && is_equal(members, rhs.members)
            && is_equal(functions, rhs.functions) && is_equal(parameters, rhs.parameters)
            && is_equal(bindings, rhs.bindings) && is_equal(binding_lists, rhs.binding_lists)
            && is_equal(files, rhs.files) && is_equal(entry_points, rhs.entry_points)
            && is_equal(diagnostics, rhs.diagnostics) && builtins == rhs.builtins;
    }
};
