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
#include <shaped-graphics-language/syntax/ids.hh>
#include <shaped-graphics-language/tokens/token.hh>

/// Everything the syntactic phases know about one file, as one value.
///
/// Every node is an id into one of these arrays and every span points into `source`, so the value copies,
/// compares and moves as a whole and holds no pointer into itself.
/// Each phase fills its own arrays and reads only the earlier ones.
struct sgl::parsed_file
{
    cc::string source;
    cc::vector<line> lines;
    cc::vector<token> tokens;
    cc::vector<group> groups;
    cc::vector<form> forms;
    /// Ids of `attribute` groups, each form owning one contiguous range.
    /// An element's attributes may be written on several of its groups, and this is where they meet.
    cc::vector<group_id> form_attributes;
    cc::vector<diagnostic> diagnostics;

    /// The first top-level line, `none` for a file without lines; its `next_sibling` chain is the top level.
    line_id first_line = line_id::none;
    /// The block group holding every top-level statement; `none` until grouped.
    group_id root_block = group_id::none;
    /// The block form holding every top-level form; `none` until the form tree is built.
    form_id root_form = form_id::none;

    /// An id is the way to a node, and the id's type picks the array.
    /// The id must be valid and must name a node that exists.
    [[nodiscard]] line const& at(line_id id) const { return lines[index_of(id)]; }
    [[nodiscard]] line& at(line_id id) { return lines[index_of(id)]; }
    [[nodiscard]] token const& at(token_id id) const { return tokens[index_of(id)]; }
    [[nodiscard]] token& at(token_id id) { return tokens[index_of(id)]; }
    [[nodiscard]] group const& at(group_id id) const { return groups[index_of(id)]; }
    [[nodiscard]] group& at(group_id id) { return groups[index_of(id)]; }
    [[nodiscard]] form const& at(form_id id) const { return forms[index_of(id)]; }
    [[nodiscard]] form& at(form_id id) { return forms[index_of(id)]; }

    [[nodiscard]] cc::string_view text_of(source_span where) const
    {
        return cc::string_view(source).subview({.offset = where.offset, .size = where.length});
    }

    [[nodiscard]] cc::span<token const> tokens_of(line const& l) const
    {
        return cc::span<token const>(tokens).subspan({.offset = l.first_token, .size = l.token_count});
    }

    /// True if the token touches the token before it: same line, no whitespace between.
    /// The first token of a line is never fused.
    [[nodiscard]] bool is_fused_left(token_id id) const
    {
        return index_of(id) > 0 && at(previous(id)).where.end() == at(id).where.offset;
    }
};

namespace sgl
{

/// Runs every syntactic phase: the line tree, the tokenizer, the group pre-pass and the form parser.
/// Total: any bytes produce a value, and what was wrong with them is in `diagnostics`.
[[nodiscard]] parsed_file parse(cc::string source);
} // namespace sgl
