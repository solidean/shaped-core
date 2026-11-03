---
created: 2026-11-02
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/bigint/bigint.cc
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/limbs.cc
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/impl/limbs.hh
---

# tg::bigint: what is still behind GMP's generic C

Measured on a Zen 5 in instructions retired per call, against GMP 6.3 built with cl.exe /O2, so its generic C without assembly.
Results reused, `assign_*` and `div_mod(q, r, a, b)` against `mpz_*` into kept results:

| limbs | add tg / gmp | sub tg / gmp | mul n x n tg / gmp | div 2n / n tg / gmp |
|---|---|---|---|---|
| 1 | 70 / 114 | 75 / 119 | 70 / 126 | 165 / 332 |
| 2 | 83 / 119 | 89 / 124 | 87 / 152 | 346 / 462 |
| 3 | 114 / 122 | 136 / 128 | 227 / 269 | 692 / 978 |
| 4 | 117 / 130 | 140 / 136 | 267 / 304 | 857 / 1242 |
| 8 | 132 / 147 | 155 / 153 | 523 / 600 | 1502 / 2172 |
| 16 | 162 / 182 | 185 / 187 | 1517 / 1744 | 3351 / 4605 |
| 32 | 222 / 249 | 245 / 255 | 4981 / 5729 | 9538 / 11957 |

Division takes fewer cycles than GMP's at every size too, since its digits come from a reciprocal rather than a hardware `div`.

Done so far:
- unrolled column products to 16 x 16, and Karatsuba above;
- mulx and adc chains as inline asm on x64, sums four limbs a round, `addmul_1` and `submul_1` two;
- inline paths for one and two limbs;
- lean out-of-line paths that decide everything before the kernel and leave the slow path to their caller;
- division into kept results, by Möller and Granlund's 3/2 digit, with a register-only path for two-limb divisors.

What is open:

- **A difference of 3 to 8 limbs retires 2–8 instructions more than GMP's** (measured).
  It is the lean dispatch and `_try_difference`'s own two register saves; the compare and the normalization are a few instructions each.
- **One limb costs about 20 more than when it was the only inline lane** (measured: add 47 then, 70 now), the price of the two-limb lane.
- **Rows past the unrolled squares retire about 8 instructions per limb product**, for shapes that are neither small nor square.
  ADCX/ADOX, two carry chains at once, would take this to about 4, behind a CPU check, since ADX is not in x86-64-v3.
- **No squaring yet**: `a * a` could compute each cross product once, about half the multiplies.
- **One-limb divisors still divide by hardware `div` per limb**, ahead of GMP already (165 against 332), but a 2/1 reciprocal would take it further.
- **MSVC has no inline asm on x64**, so there every chain is the C++ fallback, not measured.
- **A 32-limb operator sum sometimes retires about 3,200 instead of about 630, by what was allocated before** (measured).
  In the counters sweep it is the add after all sizes from 1 limb; after only 8 limbs, the sub instead; after 24, 16 or 1 alone, neither.
  One call traced inside the sweep took mimalloc's `_mi_malloc_generic` instead of its fast path, about 920 instructions in all.
  Hypothesis: the mimalloc page serving the bin is full but for the one block the loop frees and takes again, so every allocation collects it on the slow path, and some do more.
  Steady state is otherwise about 300 instructions for allocation and free, mostly mimalloc.
  Node classes of cc's own above 256 bytes would make this one free-list pop; whether that is worth it is open.
