#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `member-default-init-assignment` rule.
/// A data member's in-class default initializer must use assignment form `name = …`, never brace form
/// `name{…}`. Walks the syntax tree (never a token scan). See its `.cc` for the fix boundary.
rule const& member_default_init_assignment_rule();
} // namespace scl
