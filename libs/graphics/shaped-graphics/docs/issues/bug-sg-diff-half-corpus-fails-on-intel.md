---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_arithmetic.sgl
  - test: "sg differential - every corpus entry point computes alike on every backend and route of this machine"
  - test: "sg differential - every corpus entry point computes what its committed reference says"
  - test: "sg differential - every corpus entry point computes on the GPU what SGL's interpreter says"
---

# The half corpus fails on Intel's Arc: dx12 elides a float's round trip through a half

On the Arc B580, driver 32.0.101.9150, all three differential tests fail on the half corpus that `8f626fed6` and `4fde5ca31` added, so `dev.py ship` refuses on this machine.
`half_math.sgl`'s `known intel dx12` line logs that file's divergence now; `half_arithmetic.sgl` has no such line yet, since whether `.fast` covers it is open.

## Measured

- **`half_math.sgl` on dx12, DXC and native routes alike:** 1023 of 1024 elements differ from the interpreter and from `half_math.ref`.
  `(x as half) as float` comes back as `0x3efe7280` where a half holds `0x3efe8000`: the value never lost a float's precision.
  So Intel's dx12 compiler appears to fold the float-to-half-to-float pair away, a reading not yet confirmed in the driver's output.
- **The same module on vulkan** agrees with the interpreter bit for bit; it fails only the cross-backend comparison against dx12.
- **`half_arithmetic.sgl`, on dx12 and vulkan, both routes:** 12 of 1024 elements beyond relative 0.001, the same 12 on all four.
  For example `[115]` is `-0.1361084` against the interpreter's `-0.140625`, and `[259]` is `-4.9921875` against `-4.984375`.
  Whether `4fde5ca31`'s `.fast` allowance should cover these, a half operation computed at float precision, is not checked.

## Not known

- Whether a conversion the source writes may be elided at all: EVAL-96 says it rounds once, to nearest, at every level, which the dx12 result breaks.
  If that holds, this is Intel's bug, worked around the way `bug-sgl-half-conversion-rounds-toward-zero-on-amd` handles AMD's, or by a device-keyed tolerance.
