---
created: 2026-11-02
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/bigint/bigint.cc
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/limbs.cc
---

# tg::bigint: from two limbs up a call retires 20-50% more than GMP's generic C

Measured on a Zen 5 in instructions retired per call, against GMP 6.3 built with cl.exe /O2, so its generic C without assembly.
Results reused (`assign_*` against `mpz_*` into a kept result):

| limbs | add tg / gmp | sub tg / gmp | mul n x n tg / gmp | div 2n / n, tg operators / gmp |
|---|---|---|---|---|
| 1 | 48 / 114 | 55 / 119 | 47 / 126 | 244 / 332 |
| 2 | 162 / 119 | 185 / 124 | 180 / 152 | 648 / 462 |
| 4 | 164 / 130 | 198 / 136 | 256 / 304 | 959 / 1242 |
| 8 | 188 / 147 | 237 / 153 | 782 / 600 | 1863 / 2174 |
| 16 | 236 / 181 | 315 / 187 | 2439 / 1746 | 4807 / 4606 |
| 32 | 332 / 249 | 471 / 255 | 8922 / 5727 | 15878 / 11953 |

What is known:

- **The general sum's fixed cost is about 40 instructions over GMP's** (measured): eight callee-saved registers, operand ordering, `limbs_add`'s unroll prologue and remainder.
  A two-limb inline path, as one limb has, is the obvious next step; it costs inline code at every call site.
- **Rows past 4 x 4 retire about 9 instructions per limb product** (measured from the codegen): clang turns the 128-bit sums into `xor`/`setb`/`add`/`adc`.
  The two-row `addmul_2` gained under 2%.
  Hypothesis: MULX with ADCX/ADOX, two carry chains, needs intrinsics or assembly per platform, which is what GMP's x86 assembly does.
- **No Karatsuba yet**, which GMP uses from about 30 limbs.
- **Division has no assign form**: `div_mod` allocates both results, where `mpz_tdiv_qr` writes kept ones.
- **Past 256 bytes, 31 limbs, a fresh result takes cc's large-allocation path**: an operator's 32-limb add retires about 3,300 instructions, nearly all allocation.
  Node classes above 256 bytes would be a clean-core change.
- **orient3d through the operators at 120 bits moved from about 4,700 to 5,150 instructions** when the one-limb paths went inline, though none of its operands is one limb (measured, cause unknown).
  Hypothesis: an inlining decision changed in the operators; the disassembly of `operator-(bigint&&, bigint&&)` would show it.
