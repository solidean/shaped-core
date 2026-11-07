#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{

/// The `co-await-in-initializer` rule.
///
/// A `co_await` with an earlier element ahead of it in a brace initializer, `pair{seed(), co_await f()}`.
/// MSVC 14.44 and 14.51 crash on it: [the bug](../../../../../docs/bugs-external/msvc-coroutine-await-in-aggregate-init-crash/readme.md) has the matrix.
/// A `co_await` in the first element is quiet, since nothing is built when it suspends.
///
/// It walks tokens, because the case that kept coming back sat in a macro, and a `#define`'s body is lexed for this rule alone.
/// Tokens cannot see types, so it also reports what MSVC compiles: an element with no destructor, and a constructor's braces.
/// It stays quiet in a function's arguments, and in a lambda's body inside an initializer, which is a function of its own.
/// A macro that expands to `co_await` is out of its sight, at the call as much as in the definition.
/// A `repro.cc` under `docs/bugs-external/` is exempt, since it has to spell the bug.
///
/// No `fix`: the awaited value needs a name, which only a human picks.
rule const& co_await_in_initializer_rule();

} // namespace scl
