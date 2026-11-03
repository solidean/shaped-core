---
created: 2026-11-03
priority: next
parent: epic-tg-bigrat
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/bigint/bigint.cc
closed: 2026-11-03
resolution: fixed
---

# bigint gcd: binary for one and two limbs, Lehmer above, exact division by it

gcd was Euclid on full divisions, about a division per step.
A rational reduces by a gcd on almost every operation, so the gcd decides bigrat's speed.

Done, measured on a Zen 5 in instructions retired against GMP 6.3's generic C, into kept results:

| limbs | Euclid before | tg now | GMP mpz_gcd |
|---|---|---|---|
| 1 | 4,898 | 512 | 515 |
| 2 | 13,385 | 1,660 | 2,298 |
| 4 | 47,480 | 6,287 | 9,535 |
| 16 | - | 38,274 | 52,206 |
| 32 | - | 92,776 | 116,566 |

Binary for one limb in asm, binary on limb pairs for two, Lehmer above with at most one division a step and its buffers swapped, not copied.
A one-limb operand takes a 2/1 reciprocal remainder first, and far-apart one-limb values a division.
