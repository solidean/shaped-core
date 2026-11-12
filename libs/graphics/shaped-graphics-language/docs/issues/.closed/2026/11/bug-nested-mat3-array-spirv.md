---
created: 2026-11-01
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/plan.cc
closed: 2026-11-06
resolution: fixed
---

# A nested array of mat3 in GPU memory loses its last column through DXC's SPIR-V

HLSL holds an array of `mat3` in GPU memory as one of `column_major float4x3` (EMIT-168), which works at one dimension on every backend.
At two, `column_major float4x3 turns[2][2]` in a structured buffer's element read and wrote correctly on dx12 and on webgpu (WGSL's own `array<array<mat3x3f, 2>, 2>`).
On vulkan the same HLSL, through DXC's SPIR-V, read every element's third column as zero, and wrote the zero back; the first two columns were right.
A `float3[2, 3]` beside it, held as `float4 dirs[2][3]`, was right on vulkan too, so the nesting of matrices is what differs.

Measured: an sg compute test that read `turns[1][i % 2] * d` and wrote the struct back, 2026-11-01, on an NVIDIA GPU.
Hypothesis, unverified: DXC does not carry the matrix's majorness or stride decoration onto the inner array of an array of arrays.

Until it is understood, a multi-dimensional array of `mat3` in GPU memory is `unsupported` alike on every target.
Reading the SPIR-V needs a DXC command line, which `extern/dxc` does not install; `dxcompiler.dll` behind a small driver would do.
A fallback that avoids the question: declare the nest as one flat array of `float4x3` and index it `[i * n + j]`.
