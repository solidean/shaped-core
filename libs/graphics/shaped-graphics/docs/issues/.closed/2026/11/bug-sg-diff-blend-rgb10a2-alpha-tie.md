---
created: 2026-11-17
priority: next
tags: [differential]
refs:
  - test: sg differential - every corpus entry point computes what its committed reference says
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/blend_equations.sgl
  - file: libs/graphics/shaped-graphics/tests/differential/references/blend_equations.ref
  - file: libs/graphics/shaped-graphics/tests/differential/setups/setups.cc
closed: 2026-11-17
resolution: fixed
---

# blend_equations writes alpha 0.5 into rgb10a2's two alpha bits, a tie each vendor rounds its own way

**`blended_ps darkest` fails its committed reference on an RTX 5080, on dx12 and vulkan, through DXC and through SGL's own route alike.**
Measured 2026-11-17 at 81d00079a: 16 of 64 referenced elements are off, every one an alpha channel (51, 119, 187, 255, ...), 0.33333334 where the reference holds 0.6666667.

## Why

- `darkest` is `rgb10a2_unorm` under `min`, and its alpha has two bits: it holds 0, 1/3, 2/3 and 1 alone.
- The pixel stage writes alpha 0.5, and the clear is (0.2, 0.4, 0.6, 0.5), so both sides of the `min` are 0.5, which is 1.5 steps: an exact tie between 1/3 and 2/3.
- A float to UNORM conversion may round a tie either way, the tolerance [differential-testing.md](../../../../differential-testing.md) already grants the `targets` setup.
- The reference was written on an Intel Arc B580, which rounds it up; NVIDIA rounds it down.
- `blended`'s bound is relative 0.005, and a step of a 2-bit channel is 1/3, so no tolerance short of a step absorbs it.

The SGL interpreter test does not flag it, since only the reference test reads `darkest`'s alpha as a GPU wrote it.

## A fix

An alpha no tie reaches, on both sides of the `min`: `darkest`'s alpha 0.1 encodes to 0 on any implementation, and is the `min` of every clear the target can hold.
Layer 0 covers the whole target, so no referenced pixel keeps the clear alone.
The reference then needs rewriting, `SC_DIFFERENTIAL_REFRESH=1`, and its diff reviewed against the B580's.
