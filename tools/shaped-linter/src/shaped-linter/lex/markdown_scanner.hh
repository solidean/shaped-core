#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_span.hh>

namespace scl
{
/// What a markdown line is, at the only granularity the linter needs.
enum class markdown_line_kind : u8
{
    text,        // body text, a heading, a list item — anything a human reads as prose
    blank,       // empty or whitespace only; ends a paragraph
    fence,       // an opening or closing ``` / ~~~ line
    code,        // inside a fenced code block
    table,       // a row of a pipe table, which is data laid out in columns rather than prose
    frontmatter, // a leading --- ... --- block, delimiters included; metadata a tool consumes whole, not prose
};

/// One line of the file, tagged.
struct markdown_line
{
    markdown_line_kind kind = markdown_line_kind::text;
    source_span span; // the line's content, without its terminator
};

/// Split a markdown file into tagged lines.
///
/// Deliberately NOT a markdown parser — babel::markdown is that, and shaped-linter-core stays on
/// clean-core alone.
/// This answers one question: is this line prose, or is it code? That is what separates
/// the text a prose rule owns from a fenced block it must never touch.
///
/// Fences follow CommonMark closely enough for real files: three or more backticks or tildes, indented at
/// most three columns, closed by a fence of the same character and at least the same length, or by the end
/// of the file.
/// Indented code blocks are not recognized, matching babel::markdown.
///
/// Frontmatter is recognized only in its one unambiguous shape: line 1 is exactly `---`, and a later line is
/// exactly `---` again.
/// Without that closer the opener is a thematic break and scans as text, and a `---` anywhere but line 1 is
/// never an opener.
cc::vector<markdown_line> scan_markdown(source_buffer const& buffer);
} // namespace scl
