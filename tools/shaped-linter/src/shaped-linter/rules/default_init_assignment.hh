#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `default-init-assignment` rule.
/// A variable's initializer must use assignment form `name = …`, never brace form `name{…}` — data
/// members, function locals and namespace-scope variables alike. Walks the syntax tree (never a token
/// scan), because only the tree tells a declaration apart from a constructor's mem-initializer or an
/// aggregate at a call site. See its `.cc` for the fix boundary.
rule const& default_init_assignment_rule();
} // namespace scl
