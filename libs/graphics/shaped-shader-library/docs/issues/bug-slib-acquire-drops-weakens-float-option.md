---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_asset.cc
  - issue: bug-sg-diff-intel-ignores-precise-on-halves
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-25T19:46Z
---

# shader_asset::acquire drops weakens_float, so a WebGPU host's permission to weaken a float guarantee never reaches the compile

`shader_asset::acquire` builds the options it compiles with from the ones the entry point declares and an allowlist of compile options.
`k_float_floor_option` was missing from that list, which is how the strict-floor twin compiled at `.fast` (`bug-sg-diff-intel-ignores-precise-on-halves`); it is on the list now.
`k_weakens_float_option` is missing too.
`add_context_options` adds it for a context that weakens float guarantees, and the SGL compiler reads it, but nothing in between keeps it.

## Expected effect, not measured

A WebGPU context that allows weakening refuses an SGL entry point whose `@float` WGSL cannot keep, where it should compile it at `.fast` with a warning (EMIT-185).
The SGL compiler's own tests pass the option to the compiler directly, so none of them goes through `acquire`.

## Fix

Put `k_weakens_float_option` on the allowlist, and test an option through `acquire` rather than through the compiler alone.
Check first which webgpu tests then compile weakened, since each logs the weakening as a warning.
