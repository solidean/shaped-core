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
///
/// The fix moves each run of adjacent record definitions out of the namespace and leaves everything else inside.
/// A namespace holding nothing but definitions therefore disappears.
/// One that also holds an enum, a function or a forward declaration is split around its records, since none of those can carry a qualified name.
/// A run is what the parser's `node::follows_record` marks, so a statement the parser does not model breaks the run rather than being moved out with it.
///
/// Three shapes stay where they are and end the run around them, since a moved block cannot take them along:
/// a record declared inside a function body, an anonymous one, and the `struct S { } s;` form whose variable would land at file scope.
///
/// The rewrite compiles only where the type is already declared, normally in the library's `fwd.hh`, and a single-file linter cannot see that.
/// The same holds for the template arguments of a specialization, which are looked up where it is written.
/// It ships as a fix regardless: a missing declaration is a compile error on the very next build, which is a cheaper way to learn it than never offering the rewrite.
rule const& qualified_record_definition_rule();
} // namespace scl
