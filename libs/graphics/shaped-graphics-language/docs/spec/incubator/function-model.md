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
* **There are two lambda spellings, and `return` tells them apart.**
  The arrow lambda, `x => x + 1`, is the short one: a longer one takes a block after `=>:` and gives its value with `yield`.
  The anonymous function, `fun (x) => x + 1`, is a `fun` without a name: it may be left by `return`, and it alone takes type parameters and bindings.
* **`return` always leaves the nearest enclosing `fun`**, named or anonymous ([AST-112](../syntax/ast.md#jumps)).
  In an arrow lambda it is an error, since a reader could not tell which function it leaves.
* **Exact stack traces are possible after the fact**, because every call site is static: a fixed source id identifies it.
  [shader-logging.md](shader-logging.md) builds on this.

```sgl sketch
let halved = map(values, x => x / 2)

let bright = map(colors, c =>:
    let l = luminance c
    if l > 1 => yield c / l
    yield c
)

let first_hit = find(hits, fun (h: hit_info) -> bool:
    if not h.is_valid => return false
    return h.distance < limit
)

let larger = fun [T](a: T, b: T) => max(a, b)
```

**A parameter is a value, and `mut` makes it a place.**
A plain argument is evaluated once and the callee cannot change it.
`mut self` is the caller's place: `x.dim 0.5` changes `x`, with the index expressions of the place evaluated once.
`mut` on an ordinary parameter is the caller's place as well, and the call site must mark the argument, so an effect on a variable is visible where it happens.
The spelling of that mark is not decided.

**Operands and arguments are evaluated left to right, each exactly once.**
So a shader with two calls that have effects in one expression means the same on every target.

**Type arguments.**
`[]` after a name signals arguments that a caller may omit and have deduced:

```sgl sketch
fun sum[T](values: span[T]) -> T

let total = sum(weights)          // T deduced
let exact = sum[float](weights)   // T given
```

Compile-time functions exist as well, and a type may be passed through `()` like any value.
The difference is only deduction: a `()` parameter is never deduced, a `[]` parameter may be.
[types-as-values.md](types-as-values.md) is what makes a type an ordinary value there.

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
* `->` is used for function types and return types only, at a right-associative level of its own: `a -> b -> c` is a curried function type, and `x : (int) -> int` needs no parentheses.
* `fun (x) => e` is `=>` over a `fun` keyword form without a name, and `yield` heads a keyword form like `return`.

## Open

* How the call site marks an argument passed to a `mut` parameter, and whether `mut self` is marked by the dot alone.
* What a function value is as a type; that it is a compile-time entity is settled in [inferred-comptime.md](inferred-comptime.md).
* The rules for ray tracing function tables.
* Whether compile-time functions and `[]` parameters are one mechanism or two.
