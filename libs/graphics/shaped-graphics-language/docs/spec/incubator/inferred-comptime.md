# Compile-Time Requirements Inferred from Use

*Incubator: not normative, and one of the more experimental ideas here.*

## The idea

**A function value is a compile-time entity by definition.**
A run-time choice between functions is an error of the language.
Turning such a choice into a tag and a `switch` at every call would work, and it breaks the social performance contract: it is very much not free, and nothing at the call says so.

**That does not reject the construct, only a run-time value in it.**

```sgl sketch
let curve = case kind:
    .linear => (x => x)
    .reinhard => (x => x / (1.0 + x))
let mapped = curve value
```

This is fine when `kind` is known at compile time.
It is fine when `kind` is a `const`.
**It is even fine when `kind` is a function argument:**

```sgl sketch
fun tone_map(kind: tone_curve, value: float) -> float:
    let curve = case kind:
        .linear => (x => x)
        .reinhard => (x => x / (1.0 + x))
    return curve value
```

Calling `tone_map` with a `kind` that is known at compile time is fine, and calling it with a run-time `kind` is not.
The parameter is annotated internally as "must be known at compile time", for better diagnostics at the call.
It is Zig's `comptime` annotation, inferred from the function body and its actual use rather than written.

**This couples how a function can be used to its implementation** more than usual.
An author can make the requirement visible by putting `kind` into the `[]` list.
But `()` is an explicit argument and `[]` a deduced one, so the `()` part may well be where it belongs.

A general-purpose language would not do this, for reasons of API stability and compatibility.
A shading language has different priorities and different program shapes.
It is the same mechanism as an expression in a type position that depends on an argument, which is often a boolean or an enum ([types-as-values.md](types-as-values.md)).

It can always be built back while the language is in beta.

**A function value that is stored, passed or returned needs one static check**: none of the outside things it refers to may be out of scope where it is called.
Whether it may be returned or stored is then decided by whether the choice is static, and not by a rule about direction.

## What it touches

* The check pass: a "must be known at compile time" mark per parameter, inferred from the body, and propagated to callers.
* Inlining: a marked argument is substituted as a constant before the body is checked, which is what instantiation already does for `[]` arguments.
* Diagnostics: the error stands at the call that passes a run-time value, and it names the use inside the callee that demands a constant.
* Interfaces ([compilation-model.md](compilation-model.md)): the mark is part of a function's signature as another module sees it, although no source states it.
* The function model ([function-model.md](function-model.md)): what a function value is, and the scope check for its captures.

## Already fixed by the syntax

* `()` holds explicit arguments and `[]` deducible ones, so an author can state the requirement where wanted.
* `case` gives a value and a lambda is an expression, so the construct needs nothing new.

## Open

* Whether an inferred mark may change silently when a function's body changes, or whether a published function must state it.
* How the mark shows up in an interface without the body being compiled.
* Whether the same inference covers array sizes, loop bounds that must unroll, and resource indices on targets without binding arrays.
* What the diagnostic says when the demand sits several calls deep.
* For now, the implementation rejects a function value that is not plainly static.
