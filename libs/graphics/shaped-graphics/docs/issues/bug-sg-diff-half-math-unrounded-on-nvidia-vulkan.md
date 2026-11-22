---
created: 2026-11-22
priority: next
tags: [half, vulkan, nvidia]
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - test: sg differential - every corpus entry point*
  - issue: bug-sg-diff-half-corpus-fails-on-intel
  - issue: bug-sgl-half-conversion-rounds-toward-zero-on-amd
---

# NVIDIA's vulkan elides a float's round trip through a half

On an RTX 4090, driver 610.88, `half_math.sgl`'s `halves` differs from its reference, from the interpreter and from dx12 on the same GPU.
It fails on vulkan's dxc and native routes alike, so `dev.py ship` refuses on this machine.

## Measured

- 1023 of 1024 elements hold a float's precision where a half's is expected: `0x3efe7280` against `0x3efe8000`, `0x3d4b8ecd` against `0x3d4b8000`.
  These are the values Intel's Arc gives on dx12 (bug-sg-diff-half-corpus-fails-on-intel).
- SGL's SPIR-V writes the round trip as it should: `OpFConvert %half` then `OpFConvert %float`, with no `RelaxedPrecision`.
  DXC's SPIR-V fails the same way, so it is the driver folding the pair, not a route.
- dx12 on the same GPU agrees with the reference, and `half_arithmetic.sgl` passes on vulkan.
- Seen at `6337cd272` on a clean checkout; whether an earlier driver passed is not known.

## Open

- Whether the `RoundingModeRTE` execution mode for 16-bit floats keeps the driver from folding, which would also fix AMD's toward-zero rounding.
- What the differential test does with a vendor's known divergence, so a machine with that GPU can still ship.
