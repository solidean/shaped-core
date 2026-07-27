#pragma once

#include <clean-core/container/span.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/report/style.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// Print the rendered report to stdout.
/// Nothing is printed when `findings` is empty.
/// All the formatting lives in `render_report` (report/renderer.hh) — this is only the write.
void report_findings(cc::span<finding const> findings, source_manager const& sm, report_style style = {});
} // namespace scl
