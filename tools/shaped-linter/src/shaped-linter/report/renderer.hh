#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/report/style.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// One finding as text: a `[rule-id] message` header, the source snippet, and its `fix:` / `help:` lines.
/// Ends with a newline.
cc::string render_finding(finding const& f, source_manager const& sm, report_style style = {});

/// A whole run: every finding in file/line order, then the rationale of each rule that fired, then a one-line summary.
/// Empty for no findings — a clean run has nothing to say here.
///
/// Pure: it reads no global state and writes nothing, so `--fix` can rewrite the files afterwards and tests can compare the exact bytes.
cc::string render_report(cc::span<finding const> findings, source_manager const& sm, report_style style = {});
} // namespace scl
