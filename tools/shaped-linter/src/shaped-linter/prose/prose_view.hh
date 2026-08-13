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
struct prose_block;
struct prose_line;
struct prose_stats;
struct prose_view;
} // namespace scl

/// Where a run of prose came from.
/// A rule that reads differently in a doc comment than in markdown body
/// text branches on this; most do not care.
enum class scl::prose_kind : scl::u8
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
struct scl::prose_line
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
struct scl::prose_block
{
    prose_kind kind = prose_kind::line_comment;
    cc::vector<prose_line> lines;
};

/// All the prose in one file, in source order.
struct scl::prose_view
{
    cc::vector<prose_block> blocks;
};

/// How much prose a file carries.
///
/// Both counts are taken over the extracted text, so a `///` marker, a `*` leader and the blank lines
/// between blocks never count.
/// A word is a whitespace-separated run, so a markdown `1.` and a bare URL each count as one.
struct scl::prose_stats
{
    isize lines = 0;
    isize words = 0;
};

namespace scl
{

/// Whether the token at `index` is a Python docstring: a triple-quoted string opening a logical line.
///
/// Every prose rule reads one as prose, so `prose apply`'s code-unchanged check must not read it as code.
/// The caller is responsible for only asking about a Python token stream.
bool is_python_docstring(cc::span<token const> tokens, isize index);

/// Extract the prose of `buffer`, reading it as `language`.
/// `tokens` is that file's token stream and is unused for markdown, which has no lexer.
prose_view extract_prose(source_buffer const& buffer, source_language language, token_stream const& tokens);

/// Count the lines and words of an extracted view.
prose_stats measure_prose(prose_view const& view);

/// Measure the prose `text` carries, read as the language `path` names — extraction and counting in one.
///
/// This is the single definition of "how much prose is in this file", shared by `prose apply --stats` and
/// `prose stats`, so the two can never drift.
/// Text that will not lex measures as empty rather than failing: the numbers are a report, never a gate.
prose_stats measure_file_prose(cc::string_view text, cc::string_view path);
} // namespace scl
