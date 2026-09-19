#pragma once

#include <clean-core/container/variant.hh>
#include <shaped-graphics-language/ast/parts.hh>

/// `let pattern`, `let pattern : type`, each optionally `= value`.
struct sgl::ast::let_stmt
{
    /// Applies to the whole pattern.
    bool is_mut = false;
    /// A `name`, a `wildcard`, or a `tuple` of patterns; anything else is an `invalid` expression.
    expr_id pattern = expr_id::none;
    expr_id type = expr_id::none;
    expr_id value = expr_id::none;

    constexpr bool operator==(let_stmt const&) const = default;
};

/// `target = value` and the compound forms; a statement and never an expression.
struct sgl::ast::assign_stmt
{
    expr_id target = expr_id::none;
    /// `=`, `+=`, …
    token_id op = token_id::none;
    expr_id value = expr_id::none;

    constexpr bool operator==(assign_stmt const&) const = default;
};

/// A whole `if` / `else if` / `else` chain, which the form tree holds as sibling forms.
/// `stmt::form` is the form of the first branch.
struct sgl::ast::if_stmt
{
    /// Never empty.
    /// Only the last branch may lack a condition — or the first, when the chain opens with a stray `else`.
    range_of<if_branch> branches;

    constexpr bool operator==(if_stmt const&) const = default;
};

/// `for name in range`
struct sgl::ast::for_stmt
{
    /// A `name` or a `wildcard`.
    expr_id variable = expr_id::none;
    /// What stands right of `in`.
    expr_id iterable = expr_id::none;
    sgl::ast::body body;

    constexpr bool operator==(for_stmt const&) const = default;
};

struct sgl::ast::while_stmt
{
    expr_id condition = expr_id::none;
    sgl::ast::body body;

    constexpr bool operator==(while_stmt const&) const = default;
};

struct sgl::ast::assert_stmt
{
    expr_id condition = expr_id::none;
    expr_id message = expr_id::none;

    constexpr bool operator==(assert_stmt const&) const = default;
};

struct sgl::ast::print_stmt
{
    expr_id message = expr_id::none;

    constexpr bool operator==(print_stmt const&) const = default;
};

/// A declaration in statement position; its attributes are the declaration's.
struct sgl::ast::decl_stmt
{
    decl_id declaration = decl_id::none;

    constexpr bool operator==(decl_stmt const&) const = default;
};

struct sgl::ast::expr_stmt
{
    expr_id value = expr_id::none;

    constexpr bool operator==(expr_stmt const&) const = default;
};

struct sgl::ast::invalid_stmt
{
    constexpr bool operator==(invalid_stmt const&) const = default;
};

struct sgl::ast::stmt
{
    form_id form = form_id::none;
    range_of<attribute> attributes;

    cc::variant<invalid_stmt, let_stmt, assign_stmt, if_stmt, for_stmt, while_stmt, assert_stmt, print_stmt, decl_stmt, expr_stmt>
        node;

    bool operator==(stmt const&) const = default;
};
