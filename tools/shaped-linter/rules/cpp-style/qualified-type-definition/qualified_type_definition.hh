#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `qualified-type-definition` rule.
/// A header defines its types with a qualified name — `struct cc::span { … };`, `enum class cc::seek_dir : u8 { … };` — rather than opening the namespace around them.
/// A class, struct, union or enum defined inside an open `namespace cc { … }` in a `.hh` is a finding.
///
/// Only a definition fires.
/// A forward declaration produces no node at all, which is what leaves every `fwd.hh` alone — and a `fwd.hh` is exactly where the declaration this rewrite depends on belongs.
///
/// `impl` and `custom` are exempt at any depth: both are namespaces a reader is meant to see opened, and neither is part of a library's public vocabulary.
/// An anonymous namespace is exempt too, having no name to qualify with.
///
/// The fix moves each run of adjacent type definitions out of the namespace and leaves everything else inside.
/// A namespace holding nothing but definitions therefore disappears.
/// One that also holds a function or a forward declaration is split around its types, since neither of those can carry a qualified name.
/// A run is what the parser's `node::follows_definition` marks, so a statement the parser does not model breaks the run rather than being moved out with it.
///
/// Four shapes stay where they are and end the run around them, since a moved block cannot take them along:
/// a type declared inside a function body, an anonymous one, the `struct S { } s;` form whose variable would land at file scope,
/// and an unscoped enum with no enum-base — `enum e { … }` cannot be declared ahead of its definition, so there is nothing for a qualified definition to refer back to.
///
/// An enum-base is the one part of a head that does NOT come along for free.
/// `enum class cc::seek_dir : u8` does not compile: the base is looked up where it is written, and a bare `u8` only ever resolved through the namespace being left.
/// So the fix qualifies the base too, with the namespace's ROOT rather than its full name — the using-directive re-exporting `cc::primitive_defines` sits there, and qualified lookup follows it.
/// Any other unqualified base is out of reach of a single-file linter, and the enum is left alone rather than rewritten on a guess.
///
/// The rewrite compiles only where the type is already declared, normally in the library's `fwd.hh`, and a single-file linter cannot see that.
/// An enum's declaration must also agree on the underlying type, which is the one way this bites beyond a plain missing declaration.
/// The same holds for the template arguments of a specialization, which are looked up where it is written.
/// It ships as a fix regardless: a missing declaration is a compile error on the very next build, which is a cheaper way to learn it than never offering the rewrite.
rule const& qualified_type_definition_rule();
} // namespace scl
