#include "prose_view.hh"

#include <clean-core/common/utility.hh>
#include <shaped-linter/lex/markdown_scanner.hh>
#include <shaped-linter/lex/source_buffer.hh>

namespace scl
{
namespace
{
bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

/// Trim `span` on both sides to the non-whitespace text inside it, and report what was cut from the left.
source_span trimmed(source_buffer const& buffer, source_span span, u32& out_leading)
{
    auto const text = buffer.span_text(span);

    isize begin = 0;
    while (begin < text.size() && is_space(text[begin]))
        ++begin;
    isize end = text.size();
    while (end > begin && is_space(text[end - 1]))
        --end;

    out_leading = u32(begin);
    return {.file_id = span.file_id, .byte_begin = span.byte_begin + u32(begin), .byte_end = span.byte_begin + u32(end)};
}

/// Append a line to `blocks`, opening a new block when the kind changed or the previous block was closed.
void push_line(cc::vector<prose_block>& blocks, prose_kind kind, bool continues, source_buffer const& buffer, source_span span)
{
    u32 leading = 0;
    auto const content = trimmed(buffer, span, leading);
    if (content.empty())
        return;

    auto const column = buffer.line_col_at(content.byte_begin).column;

    if (!continues || blocks.empty() || blocks.back().kind != kind)
        blocks.push_back({.kind = kind});

    blocks.back().lines.push_back({
        .span = content,
        .text = buffer.span_text(content),
        .indent = column - 1,
    });
}

/// Strip a line comment's marker: `//`, `///`, `//!` in C++, `#` in Python.
/// The marker is punctuation, not
/// prose, so a rule reading column positions never sees it.
source_span strip_line_marker(source_span span, cc::string_view text, source_language language)
{
    isize skip = 0;
    if (language == source_language::python)
    {
        while (skip < text.size() && text[skip] == '#')
            ++skip;
    }
    else if (text.size() >= 2 && text[0] == '/' && text[1] == '/')
    {
        skip = 2;
        while (skip < text.size() && (text[skip] == '/' || text[skip] == '!'))
            ++skip;
    }
    return {.file_id = span.file_id, .byte_begin = span.byte_begin + u32(skip), .byte_end = span.byte_end};
}

/// The 1-based line a byte offset sits on.
u32 line_of(source_buffer const& buffer, u32 offset)
{
    return buffer.line_col_at(offset).line;
}

/// Whether `text` carries `"""` or `'''` starting at `i`.
bool is_triple_quote(cc::string_view text, isize i)
{
    if (i < 0 || i + 2 >= text.size())
        return false;
    auto const c = text[i];
    return (c == '"' || c == '\'') && text[i + 1] == c && text[i + 2] == c;
}

/// Whether a Python string token is triple-quoted — past any `r` / `b` / `f` prefix.
bool is_triple_quoted(cc::string_view text)
{
    isize i = 0;
    while (i < text.size() && text[i] != '"' && text[i] != '\'')
        ++i;
    return is_triple_quote(text, i);
}

/// Whether the token at `index` opens a logical line — nothing but trivia and indentation markers behind
/// it.
/// That is what separates a docstring from a triple-quoted string used as data.
bool opens_logical_line(cc::span<token const> tokens, isize index)
{
    for (auto i = index - 1; i >= 0; --i)
    {
        auto const& t = tokens[i];
        if (t.is(token_kind::newline))
            return true;
        if (t.is(token_kind::whitespace) || t.is(token_kind::indent) || t.is(token_kind::dedent))
            continue;
        return false;
    }
    return true; // the first token of the file
}

/// C++ and Python alike: comments come out of the token stream, and adjacent line comments group.
/// A `/* … */` is one block of its own lines, with the decorative leading `*` of each continuation cut.
prose_view extract_from_tokens(source_buffer const& buffer, source_language language, token_stream const& tokens)
{
    prose_view view;
    u32 previous_line = 0; // the line of the last line-comment taken, so a gap starts a new block

    auto const all = tokens.all();
    for (isize index = 0; index < all.size(); ++index)
    {
        auto const& t = all[index];
        if (t.is(token_kind::line_comment))
        {
            auto const line = line_of(buffer, t.span.byte_begin);
            auto const body = strip_line_marker(t.span, t.text, language);
            push_line(view.blocks, prose_kind::line_comment, line == previous_line + 1, buffer, body);
            previous_line = line;
            continue;
        }

        auto const docstring = language == source_language::python && t.is(token_kind::string_literal)
                            && is_triple_quoted(t.text) && opens_logical_line(all, index);
        if (!t.is(token_kind::block_comment) && !docstring)
            continue;

        // A multi-line comment or docstring is one block, split back into the lines it occupies.
        // Only the
        // first and last line carry the delimiters, and each interior line may carry a decorative `*`.
        auto const kind = docstring ? prose_kind::docstring : prose_kind::block_comment;
        auto const first = line_of(buffer, t.span.byte_begin);
        auto const last = line_of(buffer, t.span.byte_end > 0 ? t.span.byte_end - 1 : 0);
        previous_line = 0;

        for (auto line = first; line <= last; ++line)
        {
            auto const whole = buffer.line_span(line);
            auto begin = whole.byte_begin < t.span.byte_begin ? t.span.byte_begin : whole.byte_begin;
            auto const end = whole.byte_end > t.span.byte_end ? t.span.byte_end : whole.byte_end;
            if (begin >= end)
                continue;

            // Cut the delimiters and any decorative leader, so what is left is the prose itself.
            auto const raw = buffer.span_text({.file_id = whole.file_id, .byte_begin = begin, .byte_end = end});
            isize skip = 0;
            isize stop = raw.size();
            while (skip < stop && is_space(raw[skip]))
                ++skip;

            if (docstring)
            {
                // Only the first and last line carry the `"""`; an interior line is prose start to end.
                if (line == first)
                {
                    while (skip < stop && raw[skip] != '"' && raw[skip] != '\'') // an r / b / f prefix
                        ++skip;
                    if (is_triple_quote(raw, skip))
                        skip += 3;
                }
                if (line == last && stop >= skip + 3 && is_triple_quote(raw, stop - 3))
                    stop -= 3;
            }
            else
            {
                if (skip + 1 < stop && raw[skip] == '/' && raw[skip + 1] == '*')
                    skip += 2;
                else if (skip < stop && raw[skip] == '*' && (skip + 1 >= stop || raw[skip + 1] != '/'))
                    skip += 1; // the decorative leader of a continuation line
                if (stop >= skip + 2 && raw[stop - 2] == '*' && raw[stop - 1] == '/')
                    stop -= 2;
            }

            if (skip >= stop)
                continue;
            push_line(view.blocks, kind, line != first, buffer,
                      {.file_id = whole.file_id, .byte_begin = begin + u32(skip), .byte_end = begin + u32(stop)});
        }
    }

    return view;
}

/// Markdown: every line the scanner called text.
/// A blank line, a fence or a table row ends the block.
prose_view extract_from_markdown(source_buffer const& buffer)
{
    prose_view view;
    auto continues = false;

    for (auto const& line : scan_markdown(buffer))
    {
        if (line.kind != markdown_line_kind::text)
        {
            continues = false;
            continue;
        }
        push_line(view.blocks, prose_kind::markdown_text, continues, buffer, line.span);
        continues = true;
    }

    return view;
}
} // namespace

prose_view extract_prose(source_buffer const& buffer, source_language language, token_stream const& tokens)
{
    if (language == source_language::markdown)
        return extract_from_markdown(buffer);
    return extract_from_tokens(buffer, language, tokens);
}
} // namespace scl
