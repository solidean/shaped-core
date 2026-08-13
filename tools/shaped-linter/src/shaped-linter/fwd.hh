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
enum class token_kind : u8;
enum class source_language : u8;
enum class markdown_line_kind : u8;

// config/
struct config_node;
struct config_document;
struct include_directive;
struct lint_config;
struct config_resolver;
enum class config_value_kind : u8;
enum class include_verdict : u8;

// parse/
struct node;
struct parse_diagnostic;
struct syntax_tree;
enum class node_kind : u8;
enum class record_keyword : u8;
enum class init_form : u8;
enum class decl_scope : u8;

// prose/
enum class prose_kind : u8;

// rules/
enum class severity : u8;
enum class rule_layer : u8;

// report/
struct report_style;
} // namespace scl
