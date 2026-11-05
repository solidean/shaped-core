---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_guards.cc
---

# Guard buffer element and bytes accesses

An index past the end of a buffer (EVAL-90) and a `bytes` access past its last word or off a word boundary (EVAL-97) are not guarded.
No builtin gives a buffer's length: WGSL has `arrayLength`, HLSL `GetDimensions`, and metal needs the length passed, which is an sg side.
Workgroup memory read before any write (EVAL-92) needs a shadow of what was written, and is the costliest of these.

## Done on 2026-11-05

`values.length` of a buffer or a `bytes` (CHK-460), read through `GetDimensions` in HLSL and `arrayLength` in WGSL (EMIT-195).
The `abort` and `report` profiles bound every buffer index and `bytes` access by it on dx12, vulkan and webgpu (LEGAL-66).
Left:

* metal, whose buffers carry no length until sg passes the lengths in, a buffer of sizes beside the argument table.
* An element of a binding array, whose length is `unsupported-yet`, so its index is not guarded.
* Workgroup memory read before any write (EVAL-92).
