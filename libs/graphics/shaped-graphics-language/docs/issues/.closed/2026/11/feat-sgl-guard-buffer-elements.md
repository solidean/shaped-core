---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_guards.cc
closed: 2026-11-09
resolution: fixed
---

# Guard buffer element and bytes accesses

An index past the end of a buffer (EVAL-90) and a `bytes` access past its last word or off a word boundary (EVAL-97) are not guarded.
No builtin gives a buffer's length: WGSL has `arrayLength`, HLSL `GetDimensions`, and metal needs the length passed, which is an sg side.
Workgroup memory read before any write (EVAL-92) needs a shadow of what was written, and is the costliest of these.

## Done on 2026-11-05

`values.length` of a buffer or a `bytes` (CHK-460), read through `GetDimensions` in HLSL and `arrayLength` in WGSL (EMIT-195).
The `abort` and `report` profiles bound every buffer index and `bytes` access by it on dx12, vulkan and webgpu (LEGAL-66).

Workgroup memory read before any write followed on 2026-11-06 (LEGAL-69).

## Done on 2026-11-06

An element of a binding array has its own `length` (CHK-460), and an access through one is guarded by it, its index pinned so it runs once (LEGAL-66).
A binding array's own index is guarded by its `N`, on metal too (LEGAL-65).
`define` runs no access of memory too short for it, so a read is zero, a store is dropped and an atomic gives zero (LEGAL-68).

## Done on 2026-11-09

metal: each argument buffer ends in one byte length per slot, which sg fills when it mints a group, so legalize guards buffers there as on every target (EMIT-195).
That landed in `9f50c19b4`, with sg's buffer-length test and sr's binding-array guard tests running on metal; matrix rows B4 and B11 say so since.
