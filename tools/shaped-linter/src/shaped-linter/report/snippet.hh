#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/lex/source_span.hh>
#include <shaped-linter/report/style.hh>

namespace scl
{
/// The source view of a diagnostic: a `--> path:line:col` header and a line-numbered excerpt with the labelled spans underlined.
/// This is the whole of the "point at the code" work, and it knows nothing about rules or findings — which is why a rule needs no formatting code of its own.
///
/// `labels[0]` is the primary label, underlined with `^`; the rest are secondary, underlined with `-`.
/// A label may span several lines; a long span has its middle elided.
/// Labels may sit in different files — each file gets its own header block.
/// Overlapping labels on one line are not supported and render as abutting underlines.
///
/// Returns the empty string for no labels, and otherwise text ending in a newline.
cc::string render_snippet(cc::span<label const> labels, source_manager const& sm, report_style style = {});
} // namespace scl
