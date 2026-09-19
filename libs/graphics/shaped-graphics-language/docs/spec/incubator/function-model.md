# The Function Model

*Incubator: not normative.*

## The idea

A shading language has two constraints that a general-purpose language does not: **no recursion** and **no indirect calls**.
SGL embraces them instead of hiding them.

Every function is called statically, so every function can be inlined completely — and is.
The one exception is the function tables of ray tracing, which are constrained heavily enough that they do not invalidate the model.

Functions are therefore second-class-ish, and that has pleasant consequences:

* **Nested functions need no captures.**
  A nested function may reference every name in its enclosing scope, because it is inlined where those names are live.
  There is no closure object, no capture list, no lifetime question.
* **Lambdas cost nothing.**
  `x => x + 1` passed to a function is substituted, not called through.
* **Exact stack traces are possible after the fact**, because every call site is static: a fixed source id identifies it.
  [shader-logging.md](shader-logging.md) builds on this.

**Type arguments.**
`[]` after a name signals arguments that a caller may omit and have deduced:

```sgl sketch
fun sum[T](values: span[T]) -> T

let total = sum(weights)          // T deduced
let exact = sum[float](weights)   // T given
```

Compile-time functions exist as well, and a type may be passed through `()` like any value.
The difference is only deduction: a `()` parameter is never deduced, a `[]` parameter may be.

**Call by juxtaposition** is part of the model's feel:

```sgl sketch
let n = normalize v
let c = cross a b
```

A complex argument needs parentheses or a local.
A style that nudges towards simple expressions and more named locals is wanted rather than tolerated.
This is an experiment that may be built back if it turns out to cost more than it reads.

## What it touches

* The AST phase: accepting an application with inline arguments on a non-keyword head as a call.
* Name resolution: a nested function sees its enclosing scope.
* The transpiler: inlining is mandatory, not an optimization, wherever a target cannot express the construct.
* Ray tracing targets: the function-table exception needs its own rules.

## Already fixed by the syntax

* Application by juxtaposition binds tighter than every binary operator: `foo a + bar b` is `foo(a) + bar(b)`.
* Type arguments are a fused square list, the same form as a subscript.
* `=>` is its own loose, right-associative level, so `x => y => x + y` nests as a curried lambda would.
* `->` is used for function types and return types only.

## Open

* What exactly a function value is, given that it cannot be called indirectly: a compile-time-only entity, or a type of its own.
* Whether a nested function may be returned or stored, or only passed downwards.
* The rules for ray tracing function tables.
* Whether compile-time functions and `[]` parameters are one mechanism or two.
