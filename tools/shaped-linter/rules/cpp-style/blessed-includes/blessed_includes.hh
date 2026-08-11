#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{

/// The `blessed-includes` rule.
///
/// Every angle include that is not one of ours must be blessed by a `.shaped-lint.yml` above the file —
/// standard library, platform SDK and third-party alike.
/// The default is deny: a header nobody argued for is a finding, and a `deny-include` entry differs only in
/// carrying a reason that names the replacement (`<mutex>` -> `clean-core/thread/mutex.hh`).
/// [configuration](../../../docs/configuration.md) owns the file format and the merge order.
///
/// What it deliberately leaves alone:
///  - a quoted `#include "…"`, which is a file's own sibling header
///  - an angle include with a path in it (`<clean-core/fwd.hh>`) or ending in `.hh` — those are ours,
///    generated shader headers included, and no `.h` in any SDK is spelled that way
///  - an `#include` whose header a macro spells, which a single-file linter cannot resolve
///  - every file with no config above it, which is what lets the configs adopt one library at a time
///
/// It reports where an include sits, NOT where a `std::` name is used.
/// The blessing has a second tier — `<atomic>` may appear in a header while `std::atomic` must not be
/// named — and that half is a question about symbols rather than includes.
///
/// No `fix`: swapping `<mutex>` for `clean-core/thread/mutex.hh` also rewrites every call site below it,
/// which is exactly the rewrite only a human can sign off on.
rule const& blessed_includes_rule();

} // namespace scl
