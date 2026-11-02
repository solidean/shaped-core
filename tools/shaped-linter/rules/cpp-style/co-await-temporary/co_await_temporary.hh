#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{

/// The `co-await-temporary` rule.
///
/// A reference bound to awaiting a call, `auto const& x = co_await f(...);`, in a file a `.shaped-lint.yml` denies it in.
/// `co_await` on a cc::shared_async hands back a reference into the node, and the handle is what keeps the node alive.
/// The handle `f(...)` returns is a temporary that dies at the end of the statement, so when nothing else owns the node, x dangles.
/// Holding the async first, `auto const a = f(...); auto const& x = co_await a;`, keeps it alive as long as x.
///
/// The entries name callees, so a library denies `"*"` and allows by name the getters whose node something else owns:
/// a call returning a copy of a handle an object keeps is a temporary too, and safe, which the tokens cannot tell apart.
/// [configuration](../../../docs/configuration.md) owns the entry format.
///
/// It walks tokens: a `&` or `&&` declarator, then `name = co_await`, then an expression ending in a call before the `;`.
/// A named async, a member of one, a dereference, a parenthesized name and `move` or `forward` of one are not temporaries, and stay quiet.
/// No config above the file means nothing was said, and the rule stays silent.
///
/// No `fix`: holding the async needs a name for it, which only a human picks.
rule const& co_await_temporary_rule();

} // namespace scl
