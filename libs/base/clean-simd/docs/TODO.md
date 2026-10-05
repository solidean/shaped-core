# clean-simd TODO

Running list of known follow-ups.
Add entries as we discover them, and remove them as they land.

## scalar kernel

- **cl.exe and the float min/max ternary.**
  The scalar kernel's `min` is `b < a ? b : a` per lane, which returns `a` on a tie of `+0` and `-0`.
  cl.exe returned `b` on that tie, for `max` too, as if it had swapped the operands: on x64 with VS2022 and VS2026 in CI and VS2026 locally, and on ARM64 in CI.
  Compiled as a standalone function it is right — `vminss xmm0, xmm1, xmm0` at `/O2 /arch:AVX2` — so the swap happens only once the lanes are inlined into the kernel.
  For cl.exe alone the generator emits a select on the bit patterns instead (`tools/gen_simd/scalar.py`); clang and GCC keep the ternary.
  Open: which transformation does it — a guess is the lane assignments vectorized into a packed `minps` with the operands reversed.
  Also open: whether it is a cl.exe defect worth reporting, and what the select costs there.
