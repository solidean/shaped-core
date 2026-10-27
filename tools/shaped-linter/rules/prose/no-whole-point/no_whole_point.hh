#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `no-whole-point` rule.
/// Prose says what a design achieves, rather than declaring a property to be its point.
/// It fires on `the whole point`, `the entire point`, `which is the point`, `X is the point:`, `that's the point`, `the point is to`, `the crux`.
/// It stays quiet on a literal point, which in geometry code is most of them: `the point is off the surface`, `the very point the ray hits`, `the whole point set`.
///
/// It is a heuristic, a list of phrase patterns in its `.cc`, and it carries no fix: the rewrite states what the property buys, which only a writer knows.
/// It walks the prose layer of every language, a block at a time, so a phrase wrapped across two lines is still found.
rule const& no_whole_point_rule();
} // namespace scl
