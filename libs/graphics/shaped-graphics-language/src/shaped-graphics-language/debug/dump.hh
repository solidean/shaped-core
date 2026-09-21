#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl
{
/// The line tree as text, one line per source line, indented two spaces per tree depth — which is *not* the
/// source's own indentation, so a misaligned child shows where the tree really put it.
///
///     code: print "
///       string_content: hello
///     code: "
///
/// For tests and for reading a tree by eye; the format is not stable and must not be parsed.
[[nodiscard]] cc::string dump_lines(parsed_file const& file);

/// The same tree with each line's tokens instead of its text, a `~` joining a token fused to the one before it.
///
///     code: symbol(print) quote_open(")
///       string_content: string_body(hello)
///     code: quote_close(")
[[nodiscard]] cc::string dump_tokens(parsed_file const& file);

/// The group tokens as text: one line per statement, a paren with its content inline, a block indented under its line.
/// `~` joins a group fused to the one before it, `|` starts a new element line, `{@…}` follows a group with attributes.
///
///     let m = f~(| 1 , 2 | 3)
///     if x :
///       return
[[nodiscard]] cc::string dump_groups(parsed_file const& file);

/// The form tree as s-expressions, one top-level form per line and a block indented under the form that owns it.
///
///     (run (kw kw:let id:x) op:= (run num:1 op:+ num:2))
///     (kw kw:if id:x
///       (kw kw:return))
///
/// A leaf is `kind:text`, and a form's attributes follow it as `{@name}`, or `{@name (round …)}` with its arguments.
[[nodiscard]] cc::string dump_forms(parsed_file const& file);

/// One line per diagnostic, in the order they were reported: `undelimited-string @6+1`.
[[nodiscard]] cc::string dump_diagnostics(parsed_file const& file);

/// The source rebuilt from the tree alone: every line's text and terminator, in order.
/// Equal to `file.source` for every input, which is the lossless invariant rather than a convenience.
[[nodiscard]] cc::string print_source(parsed_file const& file);
} // namespace sgl
