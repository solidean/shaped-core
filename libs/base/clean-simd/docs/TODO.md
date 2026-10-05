# clean-simd TODO

Running list of known follow-ups.
Add entries as we discover them, and remove them as they land.

## scalar kernel

- **MSVC for ARM64 and the float min/max ternary.**
  The scalar kernel's `min` is `b < a ? b : a` per lane, which returns `a` on a tie of `+0` and `-0`.
  cl.exe targeting ARM64 returned `b` on that tie in CI, for `max` too, as if it had swapped the operands.
  So on that compiler alone the generator emits a select on the bit patterns instead (`tools/gen_simd/scalar.py`), and every other compiler keeps the ternary.
  Open: whether this is a cl.exe defect worth reporting, which ARM64 instruction it actually emits, and whether the select costs anything there.
  Reproducing it needs the MSVC ARM64 toolset, which the x64 dev box does not install.
