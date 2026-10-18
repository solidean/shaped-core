---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
---

# Metal's ray-tracing pipeline falls short of DXR in four places

The spec's [raytracing.md](../spec/raytracing.md) lists them under "Per target"; none is tested on hardware here.

- **A hit's instance transforms are the identity**, since Metal hands them only under intersection tags sg's tables do not declare yet.
- **A closest hit's object ray is the world ray**, for the same reason.
- **`accept_and_end_search` acts as `accept`** in an intersection function, which can only accept or not.
- **A dispatch traces one TLAS**: the closest hit's record comes from one buffer of instance offsets, which sg binds at buffer 5.
  shaped-graphics carries the same limit from its side.
