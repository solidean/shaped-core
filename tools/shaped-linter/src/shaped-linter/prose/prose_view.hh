#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_language.hh>
#include <shaped-linter/lex/source_span.hh>
#include <shaped-linter/lex/token_stream.hh>

namespace scl
{
/// Where a run of prose came from.
/// A rule that reads differently in a doc comment than in markdown body
/// text branches on this; most do not care.
enum class prose_kind : u8
{
    line_comment,  // // … or /// … in C++, # … in Python
    block_comment, // /* … */
    docstring,     // a Python triple-quoted string opening a logical line
    markdown_text, // a line of a markdown file outside any fenced code block
};

/// One line of prose, with the marker already stripped.
///
/// `text` is the prose itself — `///  front-loaded.` yields `front-loaded.` — and `span` covers exactly
/// those bytes, so a finding's carets land on the prose and never on the comment marker.
/// Markdown markers are the exception and are kept: a `#` or a `1.` sits inline with the text, and a rule
/// that cares about one is better off seeing it than having it silently removed.
/// `indent` is how far `text` sits from the start of its line, in bytes, which is what a rule needs to
/// tell a continuation line from a fresh point.
struct prose_line
{
    source_span span;
    cc::string_view text;
    u32 indent = 0;
};

/// Consecutive prose lines that read as one unit: a run of `//` comments on adjacent lines, one block
/// comment, one docstring, one markdown paragraph.
/// Blank lines end a block.
///
/// Blocks matter because several prose rules are block-level judgements — whether a line is a short
/// orphan of the one above it can only be answered against its neighbours.
struct prose_block
{
    prose_kind kind = prose_kind::line_comment;
    cc::vector<prose_line> lines;
};

/// All the prose in one file, in source order.
struct prose_view
{
    cc::vector<prose_block> blocks;
};

/// Extract the prose of `buffer`, reading it as `language`.
/// `tokens` is that file's token stream and is unused for markdown, which has no lexer.
prose_view extract_prose(source_buffer const& buffer, source_language language, token_stream const& tokens);
} // namespace scl
