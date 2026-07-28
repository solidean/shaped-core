#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `qualified-primitive` rule. The sized aliases of `cc::primitive_defines` — `u32`, `isize`,
/// `byte`, … — are vocabulary and are always spelled bare, so any namespace qualification on them
/// (`cc::u32`, and equally `sg::u32` through a namespace that re-exports them) is a finding.
///
/// Scans the token stream for the qualified spellings and reads the tree for where the bare name is
/// reachable: a fix that drops the qualifier is only offered when a `using namespace
/// cc::primitive_defines;` is in force at that point, or the use sits inside a namespace that
/// re-exports the aliases through its own fwd.hh. Otherwise the finding carries a prose hint.
rule const& qualified_primitive_rule();
} // namespace scl
