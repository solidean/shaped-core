---
created: 2026-10-30
corpus:
  - bgfx/shaders/53-sky2/fs_sky2
---

# A for over an inclusive range, for i in a ..= b

`for dy in -1 ..= 1:` is `unsupported-yet: a for over a range that is not ..<`, though OP-8 spells `..=` and a kernel loop reads best with it.
The flat tree's `for` takes a first and an end, so `a ..= b` is `a ..< b + 1` where `b + 1` cannot overflow, and a loop with a test at its end where it can (`b` the largest `int`).

- **What needs it**: PCF and blur kernels, `for dy in -r ..= r`; it blocks fs_sky2 in shader-rosetta, with `feat-sgl-let-without-value`.
