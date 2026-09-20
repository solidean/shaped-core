#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/builtins.hh>
#include <shaped-graphics-language/check/ids.hh>
#include <shaped-graphics-language/check/symbols.hh>

/// The flat typed tree of one entry point, which is all an emitter reads besides the module's types and bindings.
///
/// Full inlining is the language's model, so nothing here is a user function, a generic or a lambda.
/// What is left: locals, structured control flow, member access, constructions, literals and calls of builtins.
/// Every node keeps the AST node it came from and the chain of call sites it was inlined through.

/// The one place a name an emitter writes comes from.
/// A minted name is free, so a collision is impossible by construction; the check pass mints the locals and an
/// emitter goes on minting from the same value.
struct sgl::check::name_mint
{
    /// Every name handed out or reserved so far.
    cc::vector<cc::string> taken;

    [[nodiscard]] bool is_taken(cc::string_view name) const;

    /// Takes `name` as it is, for a name that must not change: an entry point, a type, a target's keyword.
    /// Returns false when it was taken already, which is the caller's error to report.
    bool reserve(cc::string_view name);

    /// `desired` when it is free, else `desired_1`, `desired_2`, … and the first of them that is; the result is taken.
    /// An empty `desired` mints from `_`.
    [[nodiscard]] cc::string mint(cc::string_view desired);

    [[nodiscard]] bool operator==(name_mint const& rhs) const { return ast::impl::is_equal(taken, rhs.taken); }
};

/// The AST node a flat node came from.
struct sgl::check::origin
{
    i32 file = 0;
    /// Exactly one of the two is valid, by whether the flat node is an expression or a statement.
    ast::expr_id expr = ast::expr_id::none;
    ast::stmt_id stmt = ast::stmt_id::none;

    constexpr bool operator==(origin const&) const = default;
};

/// One call a flat node was inlined through, outermost first.
struct sgl::check::call_site
{
    i32 file = 0;
    ast::expr_id call = ast::expr_id::none;

    constexpr bool operator==(call_site const&) const = default;
};

enum class sgl::check::local_kind : sgl::u8
{
    /// The entry point's own parameter, which an emitter declares in the signature.
    parameter,
    /// Introduced by a `let`.
    let,
    /// Introduced by the check pass to evaluate something once, such as a splatted value.
    temporary,
};

struct sgl::check::flat_local
{
    local_kind kind = local_kind::let;
    /// Minted, so it is unique within the entry point and differs from every module-level name.
    cc::string name;
    type_id type = type_id::none;
    bool is_mut = false;

    bool operator==(flat_local const&) const = default;
};

/// Never part of a finished entry point; it is what a node holds before it is filled.
struct sgl::check::flat_invalid
{
    constexpr bool operator==(flat_invalid const&) const = default;
};

/// A float literal; the origin has its spelling.
struct sgl::check::flat_literal
{
    f64 value = 0;

    constexpr bool operator==(flat_literal const&) const = default;
};

struct sgl::check::flat_local_ref
{
    local_id local = local_id::none;

    constexpr bool operator==(flat_local_ref const&) const = default;
};

/// `constants.view_projection`: a member of a binding, which is a global of the target.
struct sgl::check::flat_binding_member
{
    symbol_id binding = symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;

    constexpr bool operator==(flat_binding_member const&) const = default;
};

struct sgl::check::flat_member
{
    flat_expr_id object = flat_expr_id::none;
    /// A position in the `members` of the object's struct type.
    i32 member = -1;

    constexpr bool operator==(flat_member const&) const = default;
};

/// A value of the node's struct type from one value per field, in field order.
/// A splat is gone: its fields stand here one by one.
struct sgl::check::flat_construct
{
    ast::range_of<flat_expr_id> arguments;

    constexpr bool operator==(flat_construct const&) const = default;
};

/// A call of a `@builtin` function, operators included; no other call survives inlining.
struct sgl::check::flat_call
{
    symbol_id callee = symbol_id::none;
    /// The callee's `symbol::intrinsic`, repeated so an emitter switches without a lookup.
    builtin intrinsic = builtin::none;
    ast::range_of<flat_expr_id> arguments;

    constexpr bool operator==(flat_call const&) const = default;
};

struct sgl::check::flat_expr
{
    type_id type = type_id::none;
    origin from;
    /// A range of `flat_entry_point::call_sites`; empty for a node of the entry point's own body.
    ast::range_of<call_site> inlined_through;

    cc::variant<flat_invalid, flat_literal, flat_local_ref, flat_binding_member, flat_member, flat_construct, flat_call> node;

    bool operator==(flat_expr const&) const = default;
};

/// Declares `local` and gives it its value.
struct sgl::check::flat_let
{
    local_id local = local_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_let const&) const = default;
};

/// Leaves the entry point with its result.
struct sgl::check::flat_return
{
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_return const&) const = default;
};

struct sgl::check::flat_stmt
{
    origin from;
    ast::range_of<call_site> inlined_through;

    // Assignment, `if` and the loops join here once the pass carries them.
    cc::variant<flat_let, flat_return> node;

    bool operator==(flat_stmt const&) const = default;
};

/// One entry point as one flat function.
/// A type, a symbol and a binding are ids into the `checked_module` this value stands in.
struct sgl::check::flat_entry_point
{
    stage entry_stage = stage::none;
    /// The name as written: the host asks for it, so it is never minted.
    cc::string name;
    symbol_id function = symbol_id::none;
    /// The one parameter, which is `locals[0]`.
    type_id input = type_id::none;
    type_id result = type_id::none;
    /// The bindings of the function's `{...}` list in the order written, which is what decides the pipeline layout.
    cc::vector<symbol_id> bindings;

    cc::vector<flat_local> locals;
    cc::vector<flat_expr> exprs;
    cc::vector<flat_stmt> stmts;
    cc::vector<flat_expr_id> expr_lists;
    cc::vector<flat_stmt_id> stmt_lists;
    cc::vector<call_site> call_sites;
    /// The statements of the function, in order.
    ast::range_of<flat_stmt_id> body;

    /// Holds the entry point's name, every module-level name and every local; an emitter mints the rest from it.
    name_mint names;

    [[nodiscard]] flat_local const& at(local_id id) const { return locals[index_of(id)]; }
    [[nodiscard]] flat_expr const& at(flat_expr_id id) const { return exprs[index_of(id)]; }
    [[nodiscard]] flat_stmt const& at(flat_stmt_id id) const { return stmts[index_of(id)]; }
    [[nodiscard]] cc::span<flat_expr_id const> at(ast::range_of<flat_expr_id> r) const
    {
        return ast::impl::slice(expr_lists, r);
    }
    [[nodiscard]] cc::span<flat_stmt_id const> at(ast::range_of<flat_stmt_id> r) const
    {
        return ast::impl::slice(stmt_lists, r);
    }
    [[nodiscard]] cc::span<call_site const> at(ast::range_of<call_site> r) const
    {
        return ast::impl::slice(call_sites, r);
    }

    [[nodiscard]] bool operator==(flat_entry_point const& rhs) const
    {
        using ast::impl::is_equal;
        return entry_stage == rhs.entry_stage && name == rhs.name && function == rhs.function && input == rhs.input
            && result == rhs.result && is_equal(bindings, rhs.bindings) && is_equal(locals, rhs.locals)
            && is_equal(exprs, rhs.exprs) && is_equal(stmts, rhs.stmts) && is_equal(expr_lists, rhs.expr_lists)
            && is_equal(stmt_lists, rhs.stmt_lists) && is_equal(call_sites, rhs.call_sites) && body == rhs.body
            && names == rhs.names;
    }
};
