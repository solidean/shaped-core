---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/specialization.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/raytracing-futures.md
---

# Inline target code, as inline assembly is to a compiler

The portable language is the default, and every target feature SGL has not named yet is out of reach until it does: SER, NVAPI intrinsics, a Metal-only builtin.
An escape hatch would let a function's body be written in a target's own text, as inline assembly lets C reach an instruction the compiler does not know.

The design is [specialization.md](../spec/incubator/specialization.md): `const if` over target, feature, option and generic parameter, with a body in the target's text under it.
It folds a feature branch, a generic's branch and the compiler's own per-target special cases into the same mechanism.

The first form is done: text bodies and their placeholders, a file's preamble per language, text types and the declared contract (CHK-427, CHK-428, CHK-431, CHK-432).
SER is its first user, a hit object and `reorder` in `prelude/raytracing.sgl`.

What is left is the incubator's "What it touches" list; the largest pieces:

1. The float atomic add, from its C++ special case into a specialized prelude function, which needs no sg work.
2. A buffer as a parameter, so a text body reaches more than an element or an atomic.
3. `#line` inside the text, and a preamble per API rather than per language.
4. The check pass holding SGL fallbacks to the declared contract.

Opacity micromaps are no case for it: their shader side is an ordinary ray flag and feature.
