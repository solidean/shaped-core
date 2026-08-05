#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `no-flow-prose` rule.
/// Prose is one semantic point per line, never a reflowed block — so a sentence that ends in the *middle* of a line is a finding.
/// It fires on C++ and Python comments, Python docstrings, and the body text of every markdown file.
/// Fenced code blocks and pipe tables are not prose and are never read.
///
/// This is a reminder, not a proof.
/// The value is that the rule exists where the line is, so a human or an agent that did not know the convention meets it at the moment it applies.
/// There is deliberately no fix: obeying it means modelling the prose, not splicing in a newline.
///
/// The heuristic is one sentence-ending `.` followed by a space with more text behind it on the same line.
/// See its `.cc` for what that deliberately excludes — `e.g.`, list markers, inline code — and the false-positive shapes that are known to survive.
rule const& no_flow_prose_rule();
} // namespace scl
