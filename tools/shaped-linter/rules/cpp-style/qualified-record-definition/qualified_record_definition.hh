#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `qualified-record-definition` rule.
/// A header defines its types with a qualified name — `struct cc::span { … };` — rather than opening the namespace around them.
/// A class or struct defined inside an open `namespace cc { … }` in a `.hh` is a finding.
///
/// Only a definition fires.
/// A forward declaration produces no record node at all, which is what leaves every `fwd.hh` alone — and a `fwd.hh` is exactly where the declaration this rewrite depends on belongs.
///
/// `impl` and `custom` are exempt at any depth: both are namespaces a reader is meant to see opened, and neither is part of a library's public vocabulary.
/// An anonymous namespace is exempt too, having no name to qualify with.
/// Enums are not records and never fire, so a namespace holding enums *and* records still reports only the records — the mixed case, where the type moves out and the rest stays.
///
/// The fix is offered only when the namespace body is a plain series of record definitions.
/// Anything else in there — a function, a nested namespace, a variable, a forward declaration — cannot come along, since a qualified name may not *declare* a new entity.
/// The parser answers that with `node::body_holds_records_only`, and everything it does not model clears the flag, so an unrecognized construct costs the fix rather than the correctness.
///
/// The rewrite compiles only where the type is already declared, normally in the library's `fwd.hh`, and a single-file linter cannot see that.
/// It ships as a fix regardless: a missing declaration is a compile error on the very next build, which is a cheaper way to learn it than never offering the rewrite.
rule const& qualified_record_definition_rule();
} // namespace scl
