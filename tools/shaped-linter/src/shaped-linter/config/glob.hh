#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{

/// Backslashes to forward slashes, and any repeated or trailing slash collapsed away.
/// Every path a config compares against goes through this first, so a Windows path and the glob written for
/// it are in the same alphabet.
cc::string normalize_path(cc::string_view path);

/// Match a path against a glob.
///
/// `?` is one character, `*` a run of them, and neither crosses a `/`.
/// `**` is the one that does: `src/**` is everything below `src/`, and `src/**/x.hh` also matches `src/x.hh`
/// — the `/` after a `**` is optional, which is what makes the zero-directory case work.
/// A pattern ending in `/` is shorthand for the subtree, so `tests/` and `tests/**` are the same glob.
///
/// Both arguments must already be normalized; matching is case-SENSITIVE, and a caller that wants
/// otherwise (include spellings do) lowercases both sides itself.
bool glob_matches(cc::string_view pattern, cc::string_view path);

} // namespace scl
