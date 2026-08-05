#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `qualified-primitive` rule.
/// The sized aliases of `cc::primitive_defines` — `u32`, `isize`, `byte`, … — are vocabulary and are always spelled bare.
/// Any namespace qualification on them (`cc::u32`, and equally `sg::u32` through a namespace that re-exports them) is a finding.
///
/// Scans the token stream for the qualified spellings, and reads the tree for where the bare name is reachable.
/// Where it is reachable, the fix just drops the qualifier.
/// That means under a `using namespace cc::primitive_defines;` in force at that point, or inside a namespace that re-exports the aliases through its own fwd.hh.
/// At a `.cc`'s file scope the fix drops the qualifier *and* splices the directive in after the leading `#…` block.
/// At a header's file scope there is no safe edit, so the rule stays silent and reports nothing at all.
/// Inside a named namespace it reports a prose hint instead, because the right edit belongs in that library's fwd.hh.
rule const& qualified_primitive_rule();
} // namespace scl
