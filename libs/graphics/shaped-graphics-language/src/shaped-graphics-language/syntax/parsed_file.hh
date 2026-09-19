#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/forms/form.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/groups/group.hh>
#include <shaped-graphics-language/lines/line.hh>
#include <shaped-graphics-language/source/diagnostic.hh>
#include <shaped-graphics-language/tokens/token.hh>

/// Everything the syntactic phases know about one file, as one value.
///
/// Every node is an index into one of these arrays and every span points into `source`, so the value copies,
/// compares and moves as a whole and holds no pointer into itself.
/// Each phase fills its own arrays and reads only the earlier ones.
struct sgl::parsed_file
{
    cc::string source;
    cc::vector<line> lines;
    cc::vector<token> tokens;
    cc::vector<group> groups;
    cc::vector<form> forms;
    /// Indices of `attribute` groups, each form owning one contiguous range.
    /// An element's attributes may be written on several of its groups, and this is where they meet.
    cc::vector<i32> form_attributes;
    cc::vector<diagnostic> diagnostics;

    /// The first top-level line, -1 for a file without lines; its `next_sibling` chain is the top level.
    i32 first_line = -1;
    /// The block group holding every top-level statement; -1 until grouped.
    i32 root_block = -1;
    /// The block form holding every top-level form; -1 until the form tree is built.
    i32 root_form = -1;

    [[nodiscard]] cc::string_view text_of(source_span where) const
    {
        return cc::string_view(source).subview({.offset = where.offset, .size = where.length});
    }

    [[nodiscard]] cc::span<token const> tokens_of(line const& l) const
    {
        return cc::span<token const>(tokens).subspan({.offset = l.first_token, .size = l.token_count});
    }

    /// True if `tokens[index]` touches the token before it: same line, no whitespace between.
    /// The first token of a line is never fused.
    [[nodiscard]] bool is_fused_left(isize index) const
    {
        return index > 0 && tokens[index - 1].where.end() == tokens[index].where.offset;
    }
};

namespace sgl
{

/// Runs every syntactic phase: the line tree, the tokenizer, the group pre-pass and the form parser.
/// Total: any bytes produce a value, and what was wrong with them is in `diagnostics`.
[[nodiscard]] parsed_file parse(cc::string source);
} // namespace sgl
