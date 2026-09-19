#pragma once

#include <shaped-graphics-language/fwd.hh>

enum class sgl::group_kind : sgl::u8
{
    /// One token the form parser reads as is: a symbol, an operator, a comma, a string body.
    token,
    /// A matched paren with its content as children; `token` is the opener, `close_token` the closer.
    round,
    square,
    curly,
    /// A quoted literal; its children are the token groups of its body, one per content line when multi-line.
    quoted,
    /// The lines under a block colon; its children are statements and `token` is the colon.
    block,
    /// One line with everything folded into it: its continuation lines, and the sibling lines that close what it opened.
    statement,
    /// `@name`, with its fused round list as the only child when it has arguments.
    /// Never part of a run: it hangs off the group it was attached to.
    attribute,
};

/// A group token: what the form parser sees instead of tokens.
///
/// Comments and whitespace are gone, a paren is one node with children, and lines are already folded, so a run of
/// siblings is one short flat sequence however many source lines it came from.
/// Links are indices into `parsed_file::groups`, -1 for none.
struct sgl::group
{
    group_kind kind = group_kind::token;
    /// Stored rather than derived, because a continuation line's leading `.` is fused by rule and not by position.
    bool is_fused_left = false;
    /// The first group taken from an element line, which separates elements the way a comma does.
    bool starts_line = false;
    /// On an attribute: it was written after what it belongs to, so it speaks for the whole element and never for
    /// the operand it happens to follow.
    bool is_trailing = false;

    i32 token = -1;
    /// -1 on a paren or a quoted literal that was closed by force.
    i32 close_token = -1;

    i32 first_child = -1;
    i32 next_sibling = -1;
    /// A chain of `attribute` groups through `next_sibling`, in source order.
    i32 first_attribute = -1;
};
