# The Function Model

*Incubator: not normative.*

What is specified already: a mut parameter and its `mut x` mark ([CHK-315, CHK-316](../semantics/checking.md#functions)),
a parameter of function type with a function's name or an arrow lambda handed to it ([CHK-317 to CHK-319](../semantics/checking.md#calls-and-overloads)),
and type parameters on a function, deduced at the call ([CHK-338 to CHK-341](../semantics/checking.md#generics)).
This file holds what the model still wants beyond them.

## The idea

A shading language has two constraints that a general-purpose language does not: **no recursion** and **no indirect calls**.
SGL embraces them instead of hiding them.

Every function is called statically, so every function can be inlined completely — and is.
The one exception is the callables and hit groups of ray tracing, which are called through the target's own tables and are entry points rather than functions ([raytracing](../raytracing.md)).

Functions are therefore second-class-ish, and that has pleasant consequences:

* **Nested functions need no captures.**
  A nested function may reference every name in its enclosing scope, because it is inlined where those names are live.
  There is no closure object, no capture list, no lifetime question.
* **Lambdas cost nothing.**
  `x => x + 1` passed to a function is substituted, not called through; that much is built.
* **There are two lambda spellings, and `return` tells them apart.**
  The arrow lambda, `x => x + 1`, is the short one: a longer one takes a block after `=>:` and gives its value with `yield`.
  The anonymous function, `fun (x) => x + 1`, is a `fun` without a name: it may be left by `return`, and it alone takes type parameters and bindings.
  Only the arrow lambda with an arrow body is built.
* **`return` always leaves the nearest enclosing `fun`**, named or anonymous ([AST-112](../syntax/ast.md#jumps)).
  In an arrow lambda it is an error, since a reader could not tell which function it leaves.
* **Exact stack traces are possible after the fact**, because every call site is static: a fixed source id identifies it.
  [shader-logging.md](shader-logging.md) builds on this.

```sgl sketch
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

**`mut self` is the caller's place**, as a mut parameter is: `x.dim 0.5` changes `x`, with the index expressions of the place evaluated once.
It is the one form of a place parameter that is not built.

**Type arguments may be stated.**
`[]` after a name signals arguments that a caller may omit and have deduced; deducing them is built, and stating them is `unsupported-yet`.

```sgl sketch
fun sum[T](values: span[T]) -> T

let total = sum(weights)          // T deduced
let exact = sum[float](weights)   // T given
```

Compile-time functions exist as well, and a type may be passed through `()` like any value.
The difference is only deduction: a `()` parameter is never deduced, a `[]` parameter may be.
[types-as-values.md](types-as-values.md) is what makes a type an ordinary value there.

**A type parameter may be bounded**, `fun f[A: number](…)`, so a body may do more with a value of it than hand it on.
Today a bound is `unsupported-yet`, and an opaque parameter has been enough for the prelude's traces.

**A program may declare a generic struct**, `struct pair[A]:`, as the prelude does.
Three questions stand in front of it:

* A method of the template cannot name the type parameter today, so `mixed_hit.procedural()` returns through a generic helper; a program's methods would need it in scope.
* A struct of several type parameters, and one whose argument is a value, which [types-as-values.md](types-as-values.md) makes one question.
* What a host sees of an instance in GPU memory: `slib` generates one C++ struct per SGL struct, and an instance would be a template or one struct per instance.

## What it touches

* Name resolution: a nested function sees its enclosing scope.
* The transpiler: inlining is mandatory, not an optimization, wherever a target cannot express the construct.
* The check pass: a type parameter's bound, stated type arguments, and a generic struct's type parameter in its methods.

## Already fixed by the syntax

* Application by juxtaposition binds tighter than every binary operator: `foo a + bar b` is `foo(a) + bar(b)`.
* Type arguments are a fused square list, the same form as a subscript.
* `=>` is its own loose, right-associative level, so `x => y => x + y` nests as a curried lambda would.
* `->` is used for function types and return types only, at a right-associative level of its own: `a -> b -> c` is a curried function type, and `x : (int) -> int` needs no parentheses.
* `fun (x) => e` is `=>` over a `fun` keyword form without a name, and `yield` heads a keyword form like `return`.

## Open

* Whether `mut self` is marked by the dot alone.
* Whether a function value may be stored in a local or returned, which [inferred-comptime.md](inferred-comptime.md) would allow where the choice is static.
* Whether compile-time functions and `[]` parameters are one mechanism or two.
