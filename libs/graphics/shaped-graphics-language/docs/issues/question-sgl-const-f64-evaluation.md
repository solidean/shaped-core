---
created: 2026-10-30
refs:
  - file: libs/graphics/shaped-graphics-language/docs/issues/.closed/2026/10/feat-sgl-const-expressions.md
---

# Whether a const may be evaluated in f64, and which rounding is the default

`feat-sgl-const-expressions` evaluates a computed `const` on SGL's interpreter, in f32 per operation, and writes the result to every target as literals.
That was chosen so a const equals the same expression computed at run time, bit for bit, on every target.
CHK-394's literal folding does the opposite: it folds in f64 and rounds once.

- **What f64 would buy**: a weight table, a normalization or a series summed at compile time loses less to rounding than the GPU would.
- **What it costs**: `const k = f(x)` and `f(x)` in a body can differ in their last bits, and a reader cannot see which one they have.
- **Shapes to weigh**: a per-const annotation, such as `@exact` or `@precision(.f64)`, or a default of f64 with an annotation for the run-time rounding.
  Or one rule for both consts and CHK-394's folding, which disagree today.
- **Undecided**: which is the default, and whether consts and folded literals must agree.
