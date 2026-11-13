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

## Workgroup memory (2026-11-06)

LEGAL-69 shadows a compute stage's workgroup memory with a bit per scalar, set by a store and tested by a read, under `abort` and `report`.
`report` names the place and its indices, and a GPU test reports a read of what no invocation stored on dx12, vulkan and webgpu.
`define` zeroes workgroup memory before the body instead, as WGSL does; legalize writes both prologues as statements every route lowers (LEGAL-71).

## Binding arrays and memory that holds nothing (2026-11-06)

A binding array's own index is guarded by its `N`, or by CHK-491's length, and an element of one has its own `length`.
An access through an element is a block that pins that index once, tests the buffer index against that element, and then runs (LEGAL-66).
`define` needs the same shape for every buffer access, since no index is safe in memory too short for the access: a read is zero, a store is dropped, and an atomic gives zero (LEGAL-68).

## Task and mesh stages, and the depth a guard needs (2026-11-09)

A task and a mesh stage shadow their workgroup memory as a compute stage does, which a mesh pipeline's GPU test reports on dx12 and vulkan (LEGAL-69).
A shadow that would not fit beside its memory in 16 KiB is left out with the warning `workgroup-unshadowed`, rather than in silence.
The later passes walk 44 levels against a program's 40 (CHK-268), so a guard's 4 levels always fit: a site at a program's deepest was left unguarded before, two of shaped-viewer's among them.

## Left

The children, and:


- Turning `report` on by default in a dev build, once the suite has run under it.
- Range facts that drop a guard proven to hold, such as a `for` index into an array of its own length.
