# Patterns

A real pattern language for `case`, where an arm destructures its scrutinee and binds the pieces it names.

## The idea

`case` starts with the smallest thing that works: a pattern is an expression, and an arm matches when `scrutinee == pattern`.
That is equality and nothing else — it cannot look inside a value, and it cannot give a name to a part of one.

The pattern language is the step after it.
An arm's left side becomes a shape rather than a value, and a `let` inside that shape binds what stood there:

```
(1, let x) => x + 2
```

The arm matches a pair whose first element is `1`, and `x` is the second element, in scope for the arm's body alone.
Everything the first version does stays a special case of it: a bare expression is a pattern that binds nothing, and `_` is the pattern that matches anything and binds nothing.

## What it touches

* The AST: an arm's left side stops being an ordinary expression and becomes a pattern, which needs a node family of its own.
* The check pass: a pattern is checked against the scrutinee's type rather than evaluated, and a binding enters the arm's scope.
* Exhaustiveness: today it is derived only for an enum whose arms are all constant cases.
  A pattern language is what makes it derivable in general, and it is also what makes the analysis a real one rather than a set membership test.
* Legalization: a pattern that binds cannot fold to a constant, so an arm holding one never joins a `switch`; the if-else chain is its form.

## Already fixed by the syntax

* **AST-37** says an arm's pattern is an expression, which is where a pattern family would take its place.
* **AST-36** already reads every statement of a `case` block as `pattern => result`, so the shape of an arm does not change.
* `_` is a `wildcard` node of its own rather than a name, so the pattern that matches anything is already distinct from a binding.

## Open

* Which shapes destructure: tuples are the example above, and structs, enums with payloads and arrays are each a decision.
* Whether a binding is written `let x` inside the pattern or by another spelling, and whether it may be `mut`.
* Whether a guard follows a pattern, which is the usual companion feature and a separate one.
* Whether patterns reach `let` as well, which **CHK-55** makes `unsupported-yet` today.
* What exhaustiveness means once a pattern can bind, which is the hard half of the feature rather than an extra.
