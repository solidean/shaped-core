#pragma once

#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>
#include <shaped-graphics-language/syntax/ids.hh>

/// The form tree knows literals, names, applications and operators, and nothing about `fun` or `struct`.
/// It is what a Lisp calls the reader's output: structure before meaning, which the AST phase interprets.
enum class sgl::form_kind : sgl::u8
{
    /// An operand was expected and nothing usable stood there; always accompanied by a diagnostic.
    missing,
    /// Tokens no rule could place, kept so nothing of the source is dropped from the tree.
    error,

    /// Assembled from fused tokens, sign included: `-1.5e-3f32` is one form.
    number,
    quoted,
    /// `#ff00bb`
    hash_literal,
    identifier,
    wildcard,
    /// A leaf naming a keyword, as a child of a `keyword_form`.
    keyword,
    /// A leaf naming an operator of an `operator_run`, word operators and `:` and `->` included.
    op,

    /// Paren literals; the children are the elements.
    round_list,
    square_list,
    curly_list,

    /// `.name` with nothing fused before it; `token` is the name.
    leading_dot,
    /// `object.name`; the child is the object and `token` is the name.
    member,
    /// A fused paren list applied to what precedes it; the children are the callee and the list.
    call,
    /// `head arg arg`; the children are the head and the inline arguments.
    application,

    /// `token` is the operator and the child is the operand.
    prefix_operator,
    postfix_operator,
    /// Operands and `op` leaves alternating, all of one precedence level, kept flat so a chain can be judged whole.
    /// Assignment and `=>` hold exactly two operands and nest to the right instead.
    operator_run,

    /// `keyword` leaves, then the comma-separated expressions, then a `block` when the line had one.
    /// Without keywords it is an expression that owns a block.
    keyword_form,
    /// The children are the forms of the statements, one each.
    block,
    /// Forms separated by `;` on one line.
    sequence,
};

/// Links to other forms are `form_id`s, `form_id::none` where there is nothing to link to.
struct sgl::form
{
    form_kind kind = form_kind::missing;
    /// From the first byte of the first token to the last byte of the last one; empty for `missing`.
    source_span where;
    /// The token that names a leaf, a member, an operator; `none` where no single token does.
    token_id token = token_id::none;

    form_id first_child = form_id::none;
    form_id next_sibling = form_id::none;
    /// This form's attributes, as a range of `parsed_file::form_attributes`, in source order.
    /// `first_attribute` is a position in that array and names no group; the entries it reaches are the `group_id`s.
    u32 first_attribute = 0;
    u32 attribute_count = 0;
};
