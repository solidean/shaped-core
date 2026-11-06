---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/atomics.md
closed: 2026-11-05
resolution: fixed
---

# Float atomics and 64-bit atomics

Done: `atomic[float]` on every target (bindings.md), and on 2026-11-05 `atomic[ulong]` and `atomic[long]` in a `mut buffer`, behind `shader_int64_atomics` (CHK-296).
dx12 grants it through `Int64ShaderOps` on Shader Model 6.6 and vulkan through `shaderBufferInt64Atomics`; sg's compute test runs them on both.

Later on 2026-11-05: workgroup memory's, behind `shader_shared_int64_atomics` (CHK-510), run on an AMD Radeon iGPU through vulkan, which an Intel Arc B580 lacks.
And a ulong's min and max alone, behind `shader_int64_min_max_atomics` (CHK-511), which Metal writes (EMIT-218).
Left: [feat-sgl-metal-ulong-min-max-atomics-on-hardware](feat-sgl-metal-ulong-min-max-atomics-on-hardware.md), metal's on Apple hardware.
