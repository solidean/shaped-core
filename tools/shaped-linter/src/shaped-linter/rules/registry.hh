#pragma once

#include <clean-core/container/span.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The single list of all rules — the one place a rule is registered, mirroring how the clang-tidy
/// gate config is one list of gates. Every rule is guaranteed (by construction) to carry a rationale.
cc::span<rule const> all_rules();
} // namespace scl
