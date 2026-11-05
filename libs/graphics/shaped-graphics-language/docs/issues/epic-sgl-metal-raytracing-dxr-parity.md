---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
---

# Metal's ray-tracing pipeline at parity with DXR

The spec's [raytracing.md](../spec/raytracing.md) lists where metal falls short under "Per target".
A pipeline that runs on dx12 must run the same on metal, so each gap is a child:

- [chore-sgl-metal-raytracing-on-apple-hardware](chore-sgl-metal-raytracing-on-apple-hardware.md): an Apple GPU without hardware ray tracing, and sv against dx12's images.
