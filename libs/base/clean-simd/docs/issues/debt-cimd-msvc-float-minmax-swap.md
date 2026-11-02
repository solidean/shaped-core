---
created: 2026-10-17
tags: [msvc]
refs:
  - file: libs/base/clean-simd/tools/gen_simd/scalar.py
  - symbol: cimd::scalar
---

# cl.exe swaps the scalar kernel's float min / max ternary

The scalar kernel's `min` is `b < a ? b : a` per lane, which returns `a` on a tie of `+0` and `-0`.
cl.exe returned `b` on that tie, for `max` too, as if it had swapped the operands.
Seen on x64 with VS2022 and VS2026 in CI and VS2026 locally, and on ARM64 in CI.

Compiled as a standalone function it is right — `vminss xmm0, xmm1, xmm0` at `/O2 /arch:AVX2` — so the swap happens only once the lanes are inlined into the kernel.

For cl.exe alone the generator emits a select on the bit patterns instead (`tools/gen_simd/scalar.py`); clang and GCC keep the ternary.

Open:

- Which transformation does it.
  A guess is the lane assignments vectorized into a packed `minps` with the operands reversed.
- Whether it is a cl.exe defect worth reporting.
- What the select costs there.
