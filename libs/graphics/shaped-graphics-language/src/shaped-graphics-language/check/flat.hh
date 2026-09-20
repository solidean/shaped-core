#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/builtins/ids.hh>
#include <shaped-graphics-language/check/ids.hh>
#include <shaped-graphics-language/check/symbols.hh>

/// The flat typed tree of one entry point, which is all an emitter reads besides the module's types and bindings.
///
/// Full inlining is the language's model, so nothing here is a user function, a generic or a lambda.
/// What is left: locals, structured control flow, member access, constructions, literals and calls of builtins.
/// Every node keeps the AST node it came from and the chain of call sites it was inlined through.
///
/// One data structure holds two forms.
/// The STRUCTURED form is what the check pass writes: labeled blocks that may be expressions, and `leave` from any depth.
/// The CORE form is the subset every target prints one to one, and `find_core_violation` (legalize/core.hh) defines it.
/// `legalize` (legalize/legalize.hh) takes the first to the second, and `interpret` (interpret/interpret.hh) runs both.

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
    /// Introduced by a `let`, or by a parameter of an inlined call whose argument had to be bound.
    let,
    /// Introduced by a `let mut`: mutable, and the only kind of the program's own that an assignment may name.
    var,
    /// The counter of a `for`, which its loop declares and which no statement assigns.
    index,
    /// Introduced by a pass to evaluate something once or to carry a result: a splatted value, a pin, a flag.
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

/// The name of a block or a loop, which a `leave` or a `continue` refers to.
/// No target writes it: it is for a dump, and the legalizer names a flag after it.
struct sgl::check::flat_label
{
    cc::string name;

    bool operator==(flat_label const&) const = default;
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

/// Of the prelude's type `int`.
struct sgl::check::flat_int_literal
{
    i32 value = 0;

    constexpr bool operator==(flat_int_literal const&) const = default;
};

/// Of the prelude's type `bool`.
struct sgl::check::flat_bool_literal
{
    bool value = false;

    constexpr bool operator==(flat_bool_literal const&) const = default;
};

/// One case of an enum, whose type is the enum: a value a target writes as the constant of EMIT-76.
/// The case rather than the value, since two cases may hold one value and only the case names a constant.
struct sgl::check::flat_enum_value
{
    /// A position in the `cases` of the node's enum type.
    i32 case_index = -1;

    constexpr bool operator==(flat_enum_value const&) const = default;
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
    /// The callee's `symbol::intrinsic`: the registry record an emitter and the interpreter look up.
    builtin_id intrinsic = builtin_id::none;
    /// The callee's `function_info::is_pure`, repeated for the same reason; a call that is not pure has an effect.
    bool is_pure = false;
    ast::range_of<flat_expr_id> arguments;

    constexpr bool operator==(flat_call const&) const = default;
};

struct sgl::check::flat_not
{
    flat_expr_id operand = flat_expr_id::none;

    constexpr bool operator==(flat_not const&) const = default;
};

/// Short-circuit: `rhs` is evaluated only when `lhs` is true.
struct sgl::check::flat_and
{
    flat_expr_id lhs = flat_expr_id::none;
    flat_expr_id rhs = flat_expr_id::none;

    constexpr bool operator==(flat_and const&) const = default;
};

/// Short-circuit: `rhs` is evaluated only when `lhs` is false.
struct sgl::check::flat_or
{
    flat_expr_id lhs = flat_expr_id::none;
    flat_expr_id rhs = flat_expr_id::none;

    constexpr bool operator==(flat_or const&) const = default;
};

/// `block $label { … }`, the one construct an inlined call, a value block and a `loop:` with a value all become.
/// As a statement it has no value, and as an expression its value is what a `leave $label value` gives.
/// Structured form only.
struct sgl::check::flat_block
{
    label_id label = label_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_block const&) const = default;
};

struct sgl::check::flat_expr
{
    type_id type = type_id::none;
    origin from;
    /// A range of `flat_entry_point::call_sites`; empty for a node of the entry point's own body.
    ast::range_of<call_site> inlined_through;

    cc::variant<flat_invalid,
                flat_literal,
                flat_int_literal,
                flat_bool_literal,
                flat_enum_value,
                flat_local_ref,
                flat_binding_member,
                flat_member,
                flat_construct,
                flat_call,
                flat_not,
                flat_and,
                flat_or,
                flat_block>
        node;

    bool operator==(flat_expr const&) const = default;
};

/// Declares `local` and gives it its value.
struct sgl::check::flat_let
{
    local_id local = local_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_let const&) const = default;
};

/// Declares the mutable `local`; without a `value` it holds nothing until it is assigned.
struct sgl::check::flat_var
{
    local_id local = local_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_var const&) const = default;
};

/// `place` is a `flat_local_ref` of a mutable local, or a chain of `flat_member` over one.
struct sgl::check::flat_assign
{
    flat_expr_id place = flat_expr_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_assign const&) const = default;
};

/// Records `value`, which is the one effect a program has besides its result.
/// No emitter writes it yet.
struct sgl::check::flat_print
{
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_print const&) const = default;
};

/// Evaluates `value` and drops it: a call somebody wrote for its effect alone.
struct sgl::check::flat_eval
{
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_eval const&) const = default;
};

struct sgl::check::flat_if
{
    flat_expr_id condition = flat_expr_id::none;
    ast::range_of<flat_stmt_id> then_body;
    ast::range_of<flat_stmt_id> else_body;

    constexpr bool operator==(flat_if const&) const = default;
};

/// Exits the block or the loop `target` from any depth inside it; `value` is what a block expression then is.
/// `return`, `yield` and `break value` of the source all become this, and `leave $root value` is the function's return.
/// Structured form only.
struct sgl::check::flat_leave
{
    label_id target = label_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_leave const&) const = default;
};

/// Runs `body` until something exits it.
struct sgl::check::flat_loop
{
    label_id label = label_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_loop const&) const = default;
};

/// `condition` is evaluated before every iteration, the one after a `continue` included.
struct sgl::check::flat_while
{
    label_id label = label_id::none;
    flat_expr_id condition = flat_expr_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_while const&) const = default;
};

/// `for index in first ..< end`: both bounds are evaluated ONCE, `first` before `end`, before the first iteration.
/// `index` is an `int` local of kind `index`, which holds a fresh value in every iteration.
struct sgl::check::flat_for
{
    label_id label = label_id::none;
    local_id index = local_id::none;
    flat_expr_id first = flat_expr_id::none;
    flat_expr_id end = flat_expr_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_for const&) const = default;
};

/// Starts the next iteration of the loop `target`, from any depth inside it in the structured form.
/// In the core form `target` is the innermost loop, and no `once` stands in between.
struct sgl::check::flat_continue
{
    label_id target = label_id::none;

    constexpr bool operator==(flat_continue const&) const = default;
};

/// Runs `body` once; a `break` directly inside it exits it.
/// Core form only: `do { … } while (false);` in the C-like targets.
struct sgl::check::flat_once
{
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_once const&) const = default;
};

/// Exits the innermost enclosing `once` or loop.
/// Core form only.
struct sgl::check::flat_break
{
    constexpr bool operator==(flat_break const&) const = default;
};

/// Leaves the entry point with its result, from any depth.
/// Core form; in a structured tree it means `leave $root value`, and it is how the check pass spells the entry point's own `return`.
struct sgl::check::flat_return
{
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_return const&) const = default;
};

struct sgl::check::flat_stmt
{
    origin from;
    ast::range_of<call_site> inlined_through;

    // `switch` joins here with `case`; it will capture a `break` the way a loop does.
    cc::variant<flat_let,
                flat_var,
                flat_assign,
                flat_print,
                flat_eval,
                flat_if,
                flat_block,
                flat_leave,
                flat_loop,
                flat_while,
                flat_for,
                flat_continue,
                flat_once,
                flat_break,
                flat_return>
        node;

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
    cc::vector<flat_label> labels;
    /// The label of the function body, which a `leave` names to return; `none` for a tree that only has `flat_return`.
    label_id root = label_id::none;
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
    [[nodiscard]] flat_label const& at(label_id id) const { return labels[index_of(id)]; }
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
            && is_equal(labels, rhs.labels) && root == rhs.root && is_equal(exprs, rhs.exprs)
            && is_equal(stmts, rhs.stmts) && is_equal(expr_lists, rhs.expr_lists) && is_equal(stmt_lists, rhs.stmt_lists)
            && is_equal(call_sites, rhs.call_sites) && body == rhs.body && names == rhs.names;
    }
};
