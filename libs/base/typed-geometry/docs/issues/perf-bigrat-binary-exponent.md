---
created: 2026-11-04
priority: normal
---

# Does a bigrat with a binary exponent pay off on rationals from float inputs?

A rational over float inputs carries their powers of two: a double near 2^-1074 is a 17-limb denominator in a bigrat.
Held as (n / d) 2^e with n and d odd, the odd parts stay small, a product adds exponents, and a reduction's gcd sees only the odd parts.
The values are the same as a bigrat's, so this is a representation, not a new kind of number.

To measure first, against bigrat, in a consumer's benchmark: three-plane intersection points over double inputs and orient3d over them.
If it wins, decide between bigrat's own representation, which costs integer-valued rationals a little exponent bookkeeping,
and a separate type for float-derived rationals.
