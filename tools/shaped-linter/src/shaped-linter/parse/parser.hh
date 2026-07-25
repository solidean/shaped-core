#pragma once

#include <clean-core/error/result.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/token_stream.hh>
#include <shaped-linter/parse/syntax_tree.hh>

namespace scl
{
/// Parse a tokenized translation unit into a syntax tree.
/// Recognizes namespaces, records (class/struct/union with a body), function bodies, nested blocks,
/// lambda bodies and control-flow statements — all descended into — and the variable declarations
/// inside them with their initializer form. Everything else is skipped as opaque. Unknown macros /
/// preprocessor directives are opaque tokens; includes are not followed and macros are not expanded
/// (a later milestone). Resilient: malformed input degrades to a best-effort tree plus diagnostics
/// rather than failing.
cc::result<syntax_tree> parse(source_buffer const& buffer, token_stream const& tokens);
} // namespace scl
