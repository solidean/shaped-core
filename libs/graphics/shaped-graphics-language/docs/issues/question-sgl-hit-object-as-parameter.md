---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - test: "sgl check - a hit object is held by its let alone, whatever reaches it"
---

# May a hit object be a parameter or a result of a function

CHK-434 says any value of a hit object but the `let` of its `trace_hit` is `unsupported-yet`, and CHK-516 enforces that for what would copy one.
Two things stand against the sentence as written, and the language should say which it means.

## A parameter

`fun read(x: hit_object) -> float` checks and works today, though CHK-434 read strictly refuses it.
It cannot simply go: the prelude's own `fun hit_object.t => hit_object_t(self)` is a function with a hit object parameter.
Reading one is copy-free on `hlsl-vulkan`, measured at `-O3` and `-Od` through `sgl hit object spirv`, because a call is inlined with its arguments standing in.
But `x.invoke(mut p)` in the same function is refused, since an invocation has to find the `let` to know the payload's type.
So a parameter is half a value: it reads and regroups, and does not invoke.

The options:

- Keep it, say so in CHK-434, and teach invocation to follow an argument back to its `let`.
- Refuse it for user functions and let only the prelude's `self` through.

## A result

Under CHK-516 a function returning a hit object is refused, including ones that would have been copy-free, `fun same(x: hit_object) -> hit_object => x`.
`let h = same(a)` was already refused, so this keeps the two alike.
A helper wrapping `trace_hit` is the one natural use it rules out, `fun go(...) -> hit_object`, and that could be allowed where the function has a single return of a trace.

## What depends on the answer

A real function call of SGL, separate compilation above all, would end the inlining all of this rests on.
SPIR-V has hit objects cross a call as a `Function` pointer, which needs a parameter the writer spells by reference.
