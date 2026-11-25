---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/evaluation.md
  - issue: bug-sgl-half-conversion-rounds-toward-zero-on-amd
---

# A deterministic float mode: every target computes a float the same bits, to be specified

SGL's float levels say how far a target may depart from a float's exact meaning, and even `.ieee` leaves some departures to the target.
A conversion to a half rounds to nearest or toward zero, as the target does (EVAL-96), and a builtin is as precise as its target states (EVAL-6).
So no level makes a shader compute the same bits on every GPU, which lockstep simulation, replays, hashing of GPU results and cross-vendor tests all want.

A deterministic mode would be a level, or a compile option, under which every target computes every float the bits the interpreter computes.

## To specify first

- **Its scope:** a level beside `.ieee`, an option of the compile, or an attribute of a function, and how it composes with CHK-435's strictest-wins rule.
- **What it fixes, operation by operation:**
  - `+`, `-`, `*`, `/` and `sqrt`, correctly rounded, which D3D and Vulkan already promise at `.ieee` but for `/`;
  - no contraction and no reassociation, which `.precise` already gives;
  - denormals kept, which `.ieee` gives;
  - a conversion's rounding, to nearest, which neither D3D nor Vulkan promises, so it is rounded by hand from the float's bits;
  - a half operation rounded to binary16, never kept at a float's precision.
- **The builtins:** each either computed by the same formula on every target from correctly rounded steps, as the interpreter computes it, or refused in the mode.
  `sin`, `exp` and the rest then cost what a software formula costs; a table of which builtins are allowed is part of the spec.
- **Textures and the rasterizer:** a filtered sample's weights and an interpolated varying are each GPU's own.
  So a deterministic shader may only load texels and read flat varyings, or the mode says it covers compute alone.
- **Subgroup operations and atomics:** a reduction's order, and a float atomic's, differ per GPU; refused, or computed in a fixed order.
- **What it costs, and where it is refused:** a target that cannot keep it (WGSL keeps only `.fast`, EMIT-185) refuses the compile rather than weakening it.

## How it would be checked

The interpreter's bounding run collapses every range to its exact value in the mode, so the differential test holds every GPU to the bit.
The strict-floor twin is the shape of it already: a second compile of every corpus entry point at the mode, held exact.
