---
created: 2026-11-18
parent: epic-sgl-native-ir-targets
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer_lines.cc
  - file: docs/plans/sgl-native-debug-info-gaps.plan.md
---

# PIX's call stack names only the innermost inlined function of SGL's DXIL

Debugging sgl-cube's centre pixel in PIX 2610.28 at `full` (EMIT-267), the call stack reads `shade Line 49 at cube.sgl` and nothing under it.
RenderDoc, reading the same capture's DXIL, lists `main_ps` → `shade`, and the variables, lines and scopes are right in both.

## Measured

- PIX shows `cube.sgl`, steps its lines, and lists `n`, `i` and `light` with their values inside `shade`.
- Only the call stack lacks the outer frame: `main_ps`, which `shade` was inlined into.

## Hypothesis

PIX builds the stack from a location's `inlinedAt` chain in a form DXC writes and SGL's `DILocation`s do not quite match.
Candidates: the scope a call site's own location names, or `DISubprogram` fields DXC sets for an inlined function and SGL leaves 0.

## How to settle it

Compile an HLSL twin of cube.sgl's pixel stage with DXC's `-Zi`, capture both in PIX, and diff the two `ILDB` parts' `DILocation` and `DISubprogram` records.
A capture is made with `SC_SG_CAPTURE=pix SC_SG_CAPTURE_EPOCH=3 SC_SG_NATIVE_DEBUG_INFO=full`; replaying it needs Windows developer mode.
