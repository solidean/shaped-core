#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{

/// The `blocking-wait` rule.
///
/// A call to a blocking wait — `cc::async_blocking_get` and its siblings — in a file a `.shaped-lint.yml`
/// denies it in.
/// The configs deny them across test sources, where a test awaits instead, and allow them by name in the files whose
/// subject is the wait itself: the scheduler tests, a test standing up its own pool.
/// [configuration](../../../docs/configuration.md) owns the entry format.
///
/// It matches an identifier spelled like a denied call, qualified or not, so a comment mentioning one is left alone
/// and a using-declaration that brings one in is reported where it is called.
/// No config above the file means nothing was said, and the rule stays silent.
///
/// No `fix`: turning a blocking get into a `co_await` turns the test into a coroutine, which only a human can sign off on.
rule const& blocking_wait_rule();

} // namespace scl
