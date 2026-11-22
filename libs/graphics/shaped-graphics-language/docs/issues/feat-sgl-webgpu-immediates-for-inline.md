---
created: 2026-11-22
tags: [porting-feedback, webgpu]
---

# An @inline binding is WGSL's var<immediate> where the device has it

On WGSL an `@inline` binding is a uniform buffer at a reserved group, so it costs one of WebGPU's four groups.
A port with per-draw data across dozens of small draws needed a dynamic-offset uniform ring in its host instead.
WebGPU now has the native form: WGSL's `immediate` address space, `requires immediate_address_space; var<immediate> x: T;`, set by `pass.setImmediates(offset, data)` with no group.
Chrome shipped it in WebGPU 149 and 150 ([New in WebGPU 149-150](https://developer.chrome.com/blog/new-in-webgpu-149-150)).
A page detects it with `navigator.gpu.wgslLanguageFeatures.has('immediate_address_space')`.
wgpu's support, and Dawn's as the `webgpu` npm package carries it, are not checked here.

Wanted:

- A fill of `@inline` on WGSL (EMIT-265): `var<immediate>` where the device has it, the uniform group otherwise, so one source serves both.
- sg's webgpu backend writes an inline block with `setImmediates` under that fill, and no longer binds its group.
- The size limit WebGPU states for immediates, and what happens to a block past it.
- A GPU test under both node (Dawn) and deno (wgpu), skipped where the device lacks the feature.

The port's other WebGPU friction stands with it: positional group numbering, and one uniform block per group, leave no spare group once `@inline` takes one.
