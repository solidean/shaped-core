#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// Lines the formatter left as written around a syntax error, since what they mean is not settled.
struct sgl::format_skip
{
    /// Counted from 0, both included.
    i32 first_line = 0;
    i32 last_line = 0;
    /// The line of the error, and what it is.
    i32 error_line = 0;
    cc::string error;
};

/// A source as the formatter writes it.
struct sgl::formatted_source
{
    cc::string text;
    /// The runs of lines left as written around each syntax error, in source order and disjoint.
    cc::vector<format_skip> skipped;
    /// The lines, counted from 0, left as written because formatting them changed what the file parses to.
    /// Each is a rule of the formatter that is wrong there, never a property of the source.
    cc::vector<i32> unformatted_lines;
};

namespace sgl
{
/// How far a trailing comment moves right of its own code to line up with its neighbours' at most; a line whose code
/// ends further left starts a run of its own.
inline constexpr i32 max_comment_alignment_shift = 16;

/// `source` with SGL's spacing: one space between the tokens of a line that stand apart, a space after a comma, a colon,
/// around an arrow and around a binary operator, none inside brackets or before a comma or a colon.
/// The trailing comments of consecutive lines line up one space past the longest code among them, within
/// `max_comment_alignment_shift`.
/// Code is indented four spaces per level of the line tree, a tab included; a line a comment or a string owns moves with
/// its owner, so the comment keeps its layout and the string its value.
/// Line breaks, comment text and strings stay as written, a token that touches its neighbour keeps touching it unless
/// the spacing above says otherwise, and trailing whitespace and blank lines at the end go.
/// A syntax error leaves its block and that block's siblings as written, and the whole declaration where it stands at the
/// top level; everything else is formatted, since the line tree keeps an error to its block.
/// An operator that touches both operands is no such error, since it has one reading: the formatter spaces it.
/// The result parses to the same tree as `source`; a line whose formatting would not is left as written.
[[nodiscard]] formatted_source format_source(cc::string_view source);
} // namespace sgl
