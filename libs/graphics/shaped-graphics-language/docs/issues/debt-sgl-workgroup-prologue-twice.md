---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
---

# EMIT-229's workgroup prologue is written twice, once per route

What is wrong or wanted, and what is already known.

EMIT-248 lowers EMIT-229's workgroup prologue in the native IR, and the text route still writes it by hand, so the robustness profiles' shadow of workgroup memory exists in two implementations.
That is the shape `print` had before LEGAL-70 moved it into legalize: a behaviour every target shares, held by the routes rather than above them.

The target state is the same as print's: legalize writes the prologue as ordinary flat statements, both routes lower what it wrote, and neither knows what a prologue is.
Until then a change to the prologue must land in both places, and nothing checks that they agree beyond each route's own GPU test.
Whether the flat tree can already say it, as it could for print, is not checked.
