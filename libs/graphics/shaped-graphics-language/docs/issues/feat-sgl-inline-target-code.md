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

The design is [specialization.md](../spec/incubator/specialization.md): a function's bodies tried top to bottom under constant guards, a body in the target's text among them.
It folds a feature branch and the compiler's own per-target special cases into the same mechanism.

The steps it proposes:

1. The float atomic add, from its C++ special case into a specialized prelude function, which needs no sg work.
2. SER, a specialized hit object and `reorder`, which needs sg's feature, device extension and an SM 6.9 profile first.

Opacity micromaps are no case for it: their shader side is an ordinary ray flag and feature.
