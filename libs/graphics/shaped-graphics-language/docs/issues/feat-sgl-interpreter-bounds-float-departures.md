---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/impl/bounds.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/interpret.cc
  - file: libs/graphics/shaped-graphics/tests/differential/differential.cc
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_arithmetic.sgl
  - test: "sg differential - every corpus entry point computes on the GPU what SGL's interpreter says"
  - test: "sgl interpret bounds - *"
  - issue: bug-sg-diff-half-corpus-fails-on-intel
---

# The interpreter bounds what a GPU may compute under an entry point's float level, and the differential test holds the GPU to that range

## Why

The differential test held each GPU result to the interpreter's one exact value, within a tolerance per file or per setup.
EVAL-6 licenses departures per operation, and a tolerance is measured on the result, so the two disagree wherever an expression amplifies a departure.
`half_arithmetic.sgl`'s `a * b - a` is the case that shows it: with `b` near 1 the subtraction cancels, and a product kept at a float's precision, as `.fast` allows, lands 3% from binary16's result.
Intel's Arc does exactly that on dx12 and vulkan, 12 of 1024 elements, and the test failed although the GPU is right.
A wider tolerance would pass those and also pass real errors elsewhere; no relative tolerance expresses a per-operation license.

## What

A bounding run (`run_limits::bounds_floats`) gives every float and half leaf the range of values a GPU may give for it under the entry point's `@float` level, besides its exact value.
The differential test passes a GPU element inside its range, the file's tolerance applying from the range's nearer end.
At a level that licenses nothing the range collapses to the exact value, so a strict level checks what it did before.

Modeled, as EVAL-6 and CHK-435 license them:

- **A half operation kept at a float's precision** (`.fast`): the hull of the float result and its rounding to a half.
  A conversion is no operation and still rounds, which `half_math.sgl` holds exactly.
- **Contraction into a fused multiply-add** (`.fast`, `.specials`): an elementwise float product's range reaches its exact product, computed in a double.
- **Flushed denormals** (every level but `.ieee`): a denormal's range includes zero.

How it propagates:

- **A builtin is evaluated at the ends of its ranged arguments** and the hull taken: every combination where there are at most 64, one argument at a time otherwise.
  The first is exact for what is monotone in each argument, arithmetic and conversions among them, and both are off by second order at an extremum inside a box a few ulps wide.
- **Leaves of one origin move together.** An origin numbers the builtin and its arguments, so `x < x`, and one expression computed twice, are decided.
- **A condition the range leaves open is run both ways**, up to six in an invocation, and what the ways stored is joined.
  Past six, and at an index or an arm the range leaves open, what follows is unbounded, which the differential test reports at info.
- **The emulated form of a native construct is computed exactly.** The trace is the case: the GPU runs its own, whose precision is its own.

## Not modeled

- **Reassociation**, which is no widening of one operation; the file's tolerance still covers it.
- **Builtins' stated precision** per target (`sin`, `exp`, `rsqrt`, a division through a reciprocal): the tolerances still cover them, and a table of their bounds would replace those tolerances.
- **What a texture read, a subgroup operation, a derivative or an atomic does to a range**: each takes its arguments' exact values.

A strict-floor twin of each corpus entry point, compiled for the GPU at `.precise` and held to the exact value, is the follow-up this enables.
WGSL keeps only `.fast` (EMIT-185), so that twin runs on dx12, vulkan and metal alone.
