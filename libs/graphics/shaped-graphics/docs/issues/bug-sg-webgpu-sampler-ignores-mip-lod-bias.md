---
created: 2026-11-19
refs:
  - test: "sg - an anisotropic sampler is half way between two mips at the half level, its bias included"
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_format.cc
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-23T10:23Z
---

# webgpu drops a sampler's mip_lod_bias without a word

`sg - an anisotropic sampler is half way between two mips at the half level, its bias included` fails on emscripten-webgpu under node (Dawn).
Every case with `mip_lod_bias = -1` reads the unbiased level, at every anisotropy: 1.5 where 0.5 is expected, 2.5 where 1.5 is.

WebGPU's `GPUSamplerDescriptor` has no LOD bias, and `to_wgpu_sampler` in `webgpu_format.cc` drops the field without saying so.
A shader can still bias, through WGSL's `textureSampleBias` or `textureSampleLevel`, so the choices are:
- SGL folds a sampler's bias into its sample calls on WGSL, which needs the bias to reach the shader;
- sg refuses or warns on a nonzero bias on webgpu, and the test skips there;
- the test states that webgpu ignores the bias.

Found 2026-11-19, once the fix of `bug-sg-webgpu-blocking-drive-tests` stopped `--fail-grace` from skipping it.
That fix changed only when an idle routine registry's wait is ready, which this async test never waits on, so it presumably failed before too; unchecked.
