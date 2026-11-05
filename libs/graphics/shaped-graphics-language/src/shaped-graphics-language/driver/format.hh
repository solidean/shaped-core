#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// A source as the formatter writes it.
struct sgl::formatted_source
{
    cc::string text;
    /// The lines, counted from 0, left as written because formatting them changed what the file parses to.
    /// Each is a rule of the formatter that is wrong there, never a property of the source.
    cc::vector<i32> unformatted_lines;
};

namespace sgl
{
/// `source` with SGL's spacing: one space between the tokens of a line that stand apart, a space after a comma, around
/// an arrow and around a binary operator, none inside brackets or before a comma.
/// Indentation, line breaks, comments and strings stay as written, a token that touches its neighbour keeps touching
/// it unless the spacing above says otherwise, and trailing whitespace and blank lines at the end go.
/// The result parses to the same tree as `source`; a line whose formatting would not is left as written.
/// An error, naming the first, where `source` has syntax errors: what it means is not settled yet.
/// An operator that touches both operands is no such error, since it has one reading: the formatter spaces it.
[[nodiscard]] cc::result<formatted_source, cc::string> format_source(cc::string_view source);
} // namespace sgl
