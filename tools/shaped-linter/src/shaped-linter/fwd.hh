#pragma once

#include <clean-core/fwd.hh>

/// Bare primitive names (`isize`, `u8`, `u32`, `u64`, …) inside `scl`, matching the rest of the tree.
/// `isize` is the signed size/index type everywhere; byte offsets into source are stored as `u32`.
namespace scl
{
using namespace cc::primitive_defines;

// Declared here so each header defines its types qualified rather than opening the namespace around them.

// lex/
struct source_span;
struct line_col;
struct label;
struct resolved_location;
struct source_buffer;
struct source_manager;
struct lex_diagnostic;
struct token_stream;
struct token_cursor;

// parse/
struct node;
struct parse_diagnostic;
struct syntax_tree;

// report/
struct report_style;
} // namespace scl
