#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl
{
/// Cuts `source` into lines and nests them by indentation, and does nothing else.
///
/// A line is a child of the nearest line above it that is indented less, so `F` below is a child of `B` even though
/// it lines up with nothing:
///
///     B
///         C
///       F
///
/// No token is looked at, which is the point: comments and strings obey this tree rather than the other way round,
/// so nothing a line contains can change where the lines after it belong.
///
/// A blank line takes the parent of the next non-blank line, so blank lines between two children stay inside their
/// parent and blank lines after the last child sit outside it.
/// Every line's kind is `blank` or `code` afterwards; the tokenizer decides which `code` lines are something else.
[[nodiscard]] parsed_file build_line_tree(cc::string source);
} // namespace sgl
