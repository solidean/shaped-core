---
created: 2026-11-22
priority: next
tags: [porting-feedback]
---

# misplaced-splat on a splat of an expression offers the parenthesized form

`float4(..a + b, 1.0)` is `misplaced-splat: a splat that is not a whole element of a list`.
The refusal is right, since `..a + b` reads as `(..a) + b`.
The diagnostic is what is wrong: `float4(..(a + b), 1.0)` compiles and is what was meant, and nothing says so.

Wanted: where a splat is the left operand of a binary operator in a list element, the diagnostic says to parenthesize what it splats, `..(a + b)`, with a fix to offer.
Every other misplaced splat keeps today's text.
