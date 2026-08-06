#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `no-long-prose-line` rule.
/// Line length in prose is free, but 200 characters is the hard ceiling — a point that long almost always holds two, and wants splitting at the seam rather than wrapping.
/// It fires on C++ and Python comments, Python docstrings, and the body text of every markdown file.
///
/// The companion to no-flow-prose, and the shape that one cannot see.
/// no-flow-prose finds a reflowed block by its tell, a sentence ending mid-line; a single enormous sentence carries no interior full stop at all and is invisible to it.
///
/// Characters, not bytes — this tree's prose is full of em dashes, and a byte count would report a 190-character line as over.
/// A line whose longest unbreakable run already exceeds the ceiling is left alone, since no split can bring it under: a bare URL or a long path is not a wrapping mistake.
/// There is deliberately no fix, for the same reason no-flow-prose has none.
rule const& no_long_prose_line_rule();
} // namespace scl
