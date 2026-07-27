#include "reporter.hh"

#include <clean-core/string/print.hh>
#include <shaped-linter/report/renderer.hh>

namespace scl
{
void report_findings(cc::span<finding const> findings, source_manager const& sm, report_style style)
{
    if (findings.empty())
        return;

    cc::print(render_report(findings, sm, style));
}
} // namespace scl
