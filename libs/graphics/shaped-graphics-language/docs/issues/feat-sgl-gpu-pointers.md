---
created: 2026-11-02
parent: epic-sgl-gpu-pointers
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
---

# GPU pointers, or buffer device addresses

SGL reaches GPU memory only through bindings: buffers, `bytes`, images and binding arrays.
Vulkan has buffer device addresses and metal has device pointers, while dx12 and WebGPU have neither.

So a pointer type would be a feature dx12 and webgpu lack, which cuts against portability by default.
Before designing one, the uses should be named that a binding array of buffers does not already serve.
Candidates: per-instance geometry reached from a hit, linked structures, or data whose layout the host decides at run time.
