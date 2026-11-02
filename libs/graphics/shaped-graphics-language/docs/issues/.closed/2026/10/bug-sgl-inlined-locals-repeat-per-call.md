---
created: 2026-10-19
priority: someday
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T13:23Z
closed: 2026-10-20
resolution: fixed
---

# Inlining repeats a callee's locals per call, so one value is read and named dozens of times

The HLSL a shaped-viewer raster material emits holds one function, the entry point, with every call inlined into it.
Each generated attribute reader opens with `let block = ctx.param_buffer as int`, and each of its 44 calls is emitted with its own copy:

```hlsl
const int block_3 = int(ctx.param_buffer);
const float base_diffuse_roughness = asfloat(bindless_buffers[NonUniformResourceIndex(block_3)].Load(ctx.param_offset + 20u));
const int block_4 = int(ctx.param_buffer);
const float specular_weight = asfloat(bindless_buffers[NonUniformResourceIndex(block_4)].Load(ctx.param_offset + 24u));
```

DXC folds the copies into one value, so this costs nothing at run time that has been seen.
It costs the text: SGL's output is meant to be read, and here 44 names stand for one value.

What would fix it, roughly in order of reach:

- reuse a local whose initializer is pure and already computed from the same unchanged operands in a dominating position;
- or emit a callee as a target function rather than inlining it, where nothing forces the inline.
