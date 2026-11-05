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
