#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `no-why-it-matters` rule.
/// Prose says what follows from a fact, rather than announcing that the fact matters.
/// It fires on the stock announcements: `why this matters`, a sentence opening `This matters`, `, which matters`, `matters more than it sounds`, `here's why`.
/// It stays quiet where `matters` has a real subject, as in `the order matters` or `the count that matters`.
///
/// It is a heuristic, a list of phrase patterns in its `.cc`, and it carries no fix: the rewrite states the consequence, which only a writer knows.
/// It walks the prose layer of every language, a block at a time, so a phrase wrapped across two lines is still found.
rule const& no_why_it_matters_rule();
} // namespace scl
