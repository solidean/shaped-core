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
| 1 | 68 / 114 | 78 / 119 | 69 / 126 | 171 / 332 |
| 2 | 80 / 119 | 89 / 124 | 86 / 152 | 475 / 462 |
| 4 | 167 / 130 | 194 / 136 | 270 / 304 | 702 / 1242 |
| 8 | 187 / 147 | 215 / 153 | 527 / 600 | 1337 / 2172 |
| 16 | 227 / 181 | 254 / 187 | 1521 / 1745 | 3300 / 4606 |
| 32 | 307 / 249 | 335 / 255 | 5059 / 5726 | 10297 / 11959 |

Done so far: unrolled column products to 16 x 16 and Karatsuba above, mulx and adc chains as inline asm on x64, inline paths for one and two limbs, lean out-of-line paths, division into kept results.

What is open:

- **Sums from three limbs up retire 40–80 instructions more than GMP's** (measured).
  The out-of-line sum still saves eight registers; moving its allocation call out changed nothing (measured), so the cause is not that call.
  Hypothesis: the operand ordering and the two kernels with their tails, live at once.
- **One limb costs about 20 more than when it was the only inline lane** (measured: add 47 then, 68 now), the price of the two-limb lane.
- **Division is behind in cycles from 24 limbs**, 5,261 against 4,314 at 32, though ahead in instructions (measured).
  Each digit's estimate is a hardware `div`; GMP divides by a precomputed reciprocal, Möller and Granlund's, which trades it for multiplies.
- **Rows past the unrolled squares retire about 8 instructions per limb product**, for shapes that are neither small nor square.
  ADCX/ADOX, two carry chains at once, would take this to about 4, behind a CPU check, since ADX is not in x86-64-v3.
- **No squaring yet**: `a * a` could compute each cross product once, about half the multiplies.
- **MSVC has no inline asm on x64**, so there every chain is the C++ fallback, not measured.
- **In the counters sweep a 32-limb operator add retires about 3,300**, where a probe of the same operation, isolated or over the same 256 operand pairs, retires about 620 (measured, cause unknown).
  Allocation and free are about 300 of those 620, mostly mimalloc, so node classes above 256 bytes are not needed for steady state.
