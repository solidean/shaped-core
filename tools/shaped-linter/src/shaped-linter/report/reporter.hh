#pragma once

#include <clean-core/container/span.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
struct report_options
{
    bool color = false;
};

/// Print the findings grouped by rule, each group led by the rule's rationale, mirroring the
/// clang-tidy gate runner's digest. Every finding line ends with its `[rule-id]` slug so output is
/// greppable. Nothing is printed when `findings` is empty.
void report_findings(cc::span<finding const> findings, source_manager const& sm, report_options opts = {});
} // namespace scl
