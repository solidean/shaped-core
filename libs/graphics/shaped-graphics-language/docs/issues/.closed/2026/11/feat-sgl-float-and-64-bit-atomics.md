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

Left: 64-bit atomics in workgroup memory, which both APIs make optional, and an Intel Arc B580 lacks; [incubator/atomics.md](../../../../spec/incubator/atomics.md) has the row.
Metal's ulong min and max alone would be a partial feature of their own.
