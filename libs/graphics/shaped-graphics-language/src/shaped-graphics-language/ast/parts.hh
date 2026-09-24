#pragma once

#include <shaped-graphics-language/ast/ids.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>
#include <shaped-graphics-language/syntax/ids.hh>

/// What nodes of all three families are made of: the list elements and the pieces that are no node of their own.

/// `@name` or `@name(arguments)`, kept on what it was written on and never judged by name.
struct sgl::ast::attribute
{
    /// The `attribute` group, which is where the `@name` token is found.
    group_id group = group_id::none;
    /// Without the `@`.
    source_span name;
    /// The round list form the arguments came from, `none` for a bare `@name`; `@name()` has a list and no arguments.
    form_id list = form_id::none;
    range_of<argument> arguments;

    constexpr bool operator==(attribute const&) const = default;
};

/// One element of a paren list: of a call, an index, a tuple, an array, an object, or a sampler's settings.
struct sgl::ast::argument
{
    form_id form = form_id::none;
    /// Only a binding entry of a function may carry attributes; on any other element they are kept and reported.
    range_of<attribute> attributes;
    /// Empty for a positional element.
    /// `name = value` directly inside a paren list is a name and never an assignment.
    source_span name;
    /// `none` exactly when `is_shorthand`.
    expr_id value = expr_id::none;
    /// `..value`: the elements of `value` stand here instead of `value` itself.
    bool is_splat = false;
    /// A bare name in an object, `{a}`, which means `a = a` and is not expanded.
    bool is_shorthand = false;

    constexpr bool operator==(argument const&) const = default;
};

/// `name: type = default`, the one node for struct fields, binding members, parameters of every kind and the
/// members of a `struct_type`.
/// Whether a missing type or a default is acceptable is a rule of the owner, which a later phase applies.
struct sgl::ast::field
{
    form_id form = form_id::none;
    /// `_` for a parameter nobody reads, empty for the unnamed parameter of a `function_type` or a field that did not fit.
    source_span name;
    /// `mut self`; nothing else may be `mut` today, and the builder records it wherever it was written.
    bool is_mut = false;
    /// A type position; `none` when no type was written, as for `self` and most lambda parameters.
    expr_id type = expr_id::none;
    expr_id default_value = expr_id::none;
    range_of<attribute> attributes;

    constexpr bool operator==(field const&) const = default;
};

enum class sgl::ast::body_kind : sgl::u8
{
    /// Nothing was written, which is a valid signature-only declaration and an error everywhere else.
    none,
    /// The lines under a block colon.
    block,
    /// The right side of `=>`.
    arrow,
};

/// What a function, a lambda, a property, a `case` arm or a control statement runs.
/// An arrow body holds a value where the owner yields one, and a single statement where it does not:
/// `fun f() => x` has a `value`, `if done => return` has one entry in `statements`.
/// A block body never has a `value`: what it hands on is said by a `yield`, a `return` or a `break` inside it.
struct sgl::ast::body
{
    body_kind kind = body_kind::none;
    /// The block form, or the form right of `=>`.
    form_id form = form_id::none;
    range_of<stmt_id> statements;
    expr_id value = expr_id::none;

    constexpr bool operator==(body const&) const = default;
};

/// `path = value`, one line of a `pipeline` block.
/// The left side is a place rather than a name, `color_targets.albedo.blend`, which is why this is no `argument`.
struct sgl::ast::setting
{
    form_id form = form_id::none;
    /// A name or a `member` chain; an `invalid` expression for a left side that is neither.
    expr_id path = expr_id::none;
    expr_id value = expr_id::none;

    constexpr bool operator==(setting const&) const = default;
};

/// `pattern => result`, one line of a `case` block.
struct sgl::ast::case_arm
{
    form_id form = form_id::none;
    /// An ordinary expression: names, literals, `.point`, `_`, `a or b`.
    /// `none` for a line that is no arm, whose form is kept by the `invalid` value of `result`.
    expr_id pattern = expr_id::none;
    body result;

    constexpr bool operator==(case_arm const&) const = default;
};

/// One `if`, `else if` or `else` of a chain.
struct sgl::ast::if_branch
{
    form_id form = form_id::none;
    /// `none` on the closing `else`, and on a stray `else`, which is the first branch of a chain of its own.
    expr_id condition = expr_id::none;
    body then;

    constexpr bool operator==(if_branch const&) const = default;
};
