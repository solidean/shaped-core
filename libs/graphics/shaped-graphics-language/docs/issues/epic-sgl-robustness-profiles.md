---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_guards.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/legalization.md
  - test: "sgl guards - *"
---

# Robustness profiles: what a shader does at an operation with no value

SGL leaves an operation with no value undefined by default (EVAL-84, EVAL-90), since defining it costs every shader for a case correct ones never meet.
A robustness profile says what a build does there instead, `SC_SHADER_UNDEFINED` for slib's SGL edge and `sgl emit --undefined`:

- `none`, the default: the target's undefined behaviour, at no cost.
- `report`, for development: the operation and its operands are logged as a failed `assert` is, at the `.sgl` line, and the invocation ends.
  No other stack finds a division by zero or an index past an array this way, portably and at the source line; the API validation layers catch out-of-bounds memory, per API.
- `abort`: the invocation ends, with no debug window and no logging, so nothing accidentally works.
- `defined`, later: every such operation gets a value, as WGSL gives one, for a shader that must survive input it cannot trust.

## Done (2026-11-05)

Rule G1 (LEGAL-65) guards integer `/` and `%` of every shape, scalar, vector and spread, and array indices.
A guard before a barrier, a derivative or a subgroup operation sets a flag and a safe operand instead of ending the invocation.
The invocation ends after the last top-level statement that holds one, so a guard never triggers UB of its own.
The random-program differential runs under both profiles, and a GPU test reports a division on dx12, vulkan and webgpu.

A lowered `assert` before such a statement takes the same flag, so a dev build's asserts trigger no UB either.

## The first sweep (2026-11-05)

Every SGL shader of slib, sg and sr compiled under `report` and ran on dx12: no guard reported a division by zero or an index past an array.
It found two bugs a package shader with a print or an assert would have met as well, both fixed.
slib's reflection check refused sg's debug window, and one line inlined along two call chains got one debug-site id.
Left from it is [bug-sg-debug-readback-outlives-a-test](../../../shaped-graphics/docs/issues/bug-sg-debug-readback-outlives-a-test.md).
And [bug-sv-guarded-raster-pipeline-stalls-intel-driver](../../../shaped-viewer/docs/issues/bug-sv-guarded-raster-pipeline-stalls-intel-driver.md), which stopped sv's part of the sweep.

## Left

The children, and:

- A site nested past the legalizer's depth limit is left unguarded, silently.

- Turning `report` on by default in a dev build, once the suite has run under it.
- Range facts that drop a guard proven to hold, such as a `for` index into an array of its own length.
