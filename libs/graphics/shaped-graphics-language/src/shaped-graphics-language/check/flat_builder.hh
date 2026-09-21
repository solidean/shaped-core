#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>

/// Writes a flat tree node by node: for a pass that produces one, and for a test that needs one the source cannot say yet.
///
/// Every method appends and returns the id; nothing is checked, so a tree built here is as well typed as its author.
/// A statement is not part of any list until `stmt_list` or `set_body` names it, which is what lets a body be built first.
/// A span given to a method must not alias the tree, since the tree grows while it is read.
struct sgl::check::flat_builder
{
    struct signature
    {
        stage entry_stage = stage::pixel;
        cc::string_view name = "main";
        cc::string_view parameter_name = "p";
        type_id input = type_id::none;
        type_id result = type_id::none;
    };

    struct declared
    {
        local_id local = local_id::none;
        flat_stmt_id stmt = flat_stmt_id::none;
    };

    checked_module const& m;
    flat_entry_point e;
    /// What every node added from here on is attributed to.
    origin from;
    ast::range_of<call_site> inlined_through;

    /// An entry point with its parameter as `locals[0]`, a root label, and every module-level name taken in the mint.
    [[nodiscard]] static flat_builder create(checked_module const& m, signature const& s);

    /// Goes on writing into `e`, which keeps everything it holds.
    [[nodiscard]] static flat_builder extend(checked_module const& m, flat_entry_point e);

    /// The type of the struct named `name`, builtin or not: `"float"`, `"frag"`; `none` when there is none.
    [[nodiscard]] type_id type_named(cc::string_view name) const;

    /// `desired` goes through the mint; a `var` is mutable and nothing else is.
    local_id add_local(local_kind kind, cc::string_view desired, type_id type);
    /// `desired` is made unique among the labels by a numeric suffix.
    label_id add_label(cc::string_view desired);

    // ---- expressions ------------------------------------------------------------------------------------------------

    template <class Node>
    flat_expr_id add_expr(type_id type, Node node)
    {
        e.exprs.push_back({.type = type, .from = from, .inlined_through = inlined_through, .node = cc::move(node)});
        return flat_expr_id(e.exprs.size() - 1);
    }

    flat_expr_id literal(f64 value);
    flat_expr_id int_literal(i32 value);
    flat_expr_id bool_literal(bool value);
    flat_expr_id local(local_id id);
    flat_expr_id member(flat_expr_id object, i32 index);
    /// `name` must be a field of the object's type.
    flat_expr_id member(flat_expr_id object, cc::string_view name);
    flat_expr_id construct(type_id type, cc::span<flat_expr_id const> arguments);
    /// `binding.member`, of the member's type; `member` is a position in the binding's members.
    flat_expr_id binding_member(symbol_id binding, i32 member);
    /// `buffer[index]`, of the buffer's element type; `buffer` names a `buffer[T]`, today always a binding member.
    flat_expr_id buffer_element(flat_expr_id buffer, flat_expr_id index);
    /// A call of the prelude's `@builtin` function `name`, with its result type and its purity: `call("add_int", {a, b})`.
    /// Among overloads, the one whose parameter types are the arguments' types; an operator function is named by its own name.
    /// The module must declare it.
    flat_expr_id call(cc::string_view name, cc::span<flat_expr_id const> arguments);
    /// The same call as one that has an effect, which no function of the prelude is yet.
    flat_expr_id call_with_effect(cc::string_view name, cc::span<flat_expr_id const> arguments);
    flat_expr_id not_(flat_expr_id operand);
    flat_expr_id and_(flat_expr_id lhs, flat_expr_id rhs);
    flat_expr_id or_(flat_expr_id lhs, flat_expr_id rhs);
    flat_expr_id block_expr(label_id label, type_id type, cc::span<flat_stmt_id const> body);

    // ---- statements -------------------------------------------------------------------------------------------------

    template <class Node>
    flat_stmt_id add_stmt(Node node)
    {
        e.stmts.push_back({.from = from, .inlined_through = inlined_through, .node = cc::move(node)});
        return flat_stmt_id(e.stmts.size() - 1);
    }

    /// Declares a local of the value's type.
    declared let(cc::string_view desired, flat_expr_id value);
    flat_stmt_id let(local_id local, flat_expr_id value);
    /// Without a `value` the local holds nothing until it is assigned.
    declared var(cc::string_view desired, type_id type, flat_expr_id value = flat_expr_id::none);
    flat_stmt_id var(local_id local, flat_expr_id value = flat_expr_id::none);
    flat_stmt_id assign(flat_expr_id place, flat_expr_id value);
    flat_stmt_id print(flat_expr_id value);
    /// Evaluates `value` and drops it.
    flat_stmt_id eval(flat_expr_id value);
    flat_stmt_id if_(flat_expr_id condition,
                     cc::span<flat_stmt_id const> then_body,
                     cc::span<flat_stmt_id const> else_body = {});
    flat_stmt_id block(label_id label, cc::span<flat_stmt_id const> body);
    flat_stmt_id leave(label_id target, flat_expr_id value = flat_expr_id::none);
    flat_stmt_id loop(label_id label, cc::span<flat_stmt_id const> body);
    flat_stmt_id while_(label_id label, flat_expr_id condition, cc::span<flat_stmt_id const> body);
    /// `index` is a local of kind `index`, added before the body that reads it was built.
    flat_stmt_id for_(label_id label,
                      local_id index,
                      flat_expr_id first,
                      flat_expr_id end,
                      cc::span<flat_stmt_id const> body);
    flat_stmt_id continue_(label_id target);
    flat_stmt_id once(cc::span<flat_stmt_id const> body);
    flat_stmt_id break_();
    flat_stmt_id return_(flat_expr_id value);

    ast::range_of<flat_expr_id> expr_list(cc::span<flat_expr_id const> list);
    ast::range_of<flat_stmt_id> stmt_list(cc::span<flat_stmt_id const> list);
    ast::range_of<flat_arm> arm_list(cc::span<flat_arm const> list);
    void set_body(cc::span<flat_stmt_id const> list);
};
