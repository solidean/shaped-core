---
created: 2026-11-03
priority: someday
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/bigrat/bigrat.cc
---

# bigrat: a harmonic sum of 200 terms retires 15% more than GMP's mpq

Measured on a Zen 5 in instructions retired, 1/1 + ... + 1/n with `+=` on a fresh `bigrat(1, k)` a term:
n = 50 retires about 58,900 against GMP's 58,600, and n = 200 about 316,000 against 275,000.

Known:
- One term traced at n = 40 is about 790 instructions in the sum itself; the rest of a term is constructing `bigrat(bigint(1), bigint(k))`, two allocations and a reduction.
- Each sum takes gcd(den, k) and then gcd(numerator, g), both a long value against one limb, now by a 2/1 reciprocal remainder and the binary gcd.

Hypothesis, not measured: at n = 200 the denominator is about five limbs, and GMP's remainder by one limb, its mod_1 with a precomputed inverse table, is cheaper per limb than ours.
The stress test matters less than the orient3d and elimination workloads, where bigrat leads by 20 to 30 percent.
