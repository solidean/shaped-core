---
created: 2026-11-25
refs:
  - issue: feat-sgl-interpreter-bounds-float-departures
  - file: libs/graphics/shaped-graphics-language/docs/lsp.md
---

# An editor shows how a shader's precision flows, from the interpreter's ranges

A bounding run knows, for every value it computes, how far a GPU may land from the exact answer, and why: a half kept at a float's precision, a contraction, a flushed denormal.
The same run could show that to whoever is debugging a graphics problem, line by line, in the editor.

## What it would show

- **Per expression, the range's width relative to its value**, for one input: a pixel, a thread, or a test's own bindings.
  Hovering a value says its exact result, its range, and which license widened it most.
- **Where precision is lost**: the operation whose range grows the most relative to its operands', `a * b - a` cancelling where `b` is near 1 being the case that started this.
  An editor marks it the way a linter marks a warning, with the cancellation named.
- **A branch the ranges leave open**, which a GPU may decide either way: the place a picture differs between vendors, flagged before it is seen.
- **What a stricter `@float` level would change**: the same run at `.precise` beside `.fast`, so a level is chosen by what it buys.

## The shape

- `sgl lsp` already runs tests on the interpreter for an editor (lsp.md); a bounding run of a test, or of an entry point at one chosen input, feeds the same channel.
- The run records each expression's range by its flat expression's origin in the source, which the check pass already keeps for diagnostics.
- A whole image is the larger version: one run per pixel, shown as a heat map of a chosen value's range, in a debug view of the viewer or the playground.

## Open

- Which input the editor runs: a test is the easy one, since it binds its own values; an entry point needs inputs from a capture.
