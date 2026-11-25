---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_arithmetic.sgl
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - test: "sg differential - every corpus entry point computes at a precise floor what SGL's interpreter says"
  - issue: bug-sg-diff-half-corpus-fails-on-intel
---

# Intel's Arc keeps half operations at a float's precision under a precise floor, on dx12 and vulkan alike

The strict-floor twin compiles each corpus entry point at a `.precise` floor, where no target may keep a half operation at a float's precision or contract it (EVAL-6, CHK-435).
On the Arc B580, driver 32.0.101.9150, Intel's results do not change from `.fast`.

## Measured

- **`half_arithmetic.sgl`, dx12 and vulkan, DXC and native routes alike:** the same 12 of 1024 elements as at `.fast`, bit for bit.
  `[115]`, `a * b - a` with `a = 30.984375` and `b = 0.99560546875`, is `-0.1361084` where binary16's operations give `-0.140625`.
  That is the product unrounded and the difference rounded once, a half fused multiply-add.
- **`half_math.sgl`, dx12 both routes:** 1023 of 1024 elements still skip the float-to-half-to-float round trip, as `bug-sg-diff-half-corpus-fails-on-intel` measured at `.fast`.
  Vulkan rounds them at either level.

## What SGL asks for

- On vulkan SGL's own SPIR-V decorates each contractable instruction `NoContraction` once its guarantee is stricter than `.fast` (`writer_operations.cc`).
  The floor raises every instruction's guarantee, since `lower.cc` takes the entry point's.
  DXC's route asks with `-Gis`, which writes the same decoration.
- On dx12 the DXC route compiles with `-Gis` and SGL's DXIL marks the operations precise.
- So both routes ask for what the driver does not do: which is a driver bug unless a 16-bit operation is exempt somewhere, which is not checked.

## Not known

- Whether Vulkan's precision rules let a 16-bit `OpFMul` be computed wider without `RelaxedPrecision`; they read as correctly rounded.
- A standalone repro in docs/bugs-external, which this needs before a report to Intel.

`half_arithmetic.sgl` and `half_math.sgl` carry `known intel <backend> ... precise` lines for it, so the twin logs these and fails once Intel keeps the floor.
