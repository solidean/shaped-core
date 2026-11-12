---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/emit.cc
closed: 2026-11-06
resolution: fixed
---

# Carry a 64-bit value across a stage link on dx12 and metal as two words

A 64-bit scalar or two-vector crosses a stage link on vulkan alone (CHK-454, EMIT-196): DXIL refuses a 64-bit signature element, and MSL has no 64-bit varying.
Writing it as two `uint`s, and joining them in the next stage, would carry it on both, flat as it is already.
A 64-bit three- or four-vector there takes two locations, which the same split would settle.

Fixed by EMIT-196: dx12 and metal carry a scalar as a `uint2` and a two-vector as a `uint4`, and arrays of either element by element.
A three- or four-vector stays `unsupported-yet` (CHK-454) on every target, vulkan included, since it takes two locations on all of them.
