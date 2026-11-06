---
created: 2026-11-06
refs:
  - test: shaped-graphics-test
---

# shaped-graphics-test does not finish under deno and wgpu

`uv run dev.py test shaped-graphics-test --preset emscripten-webgpu-relwithdebinfo --runtime deno` ends early on this machine, for two separate reasons.
No CI job runs this combination, since a runner has no GPU, so the sg suite has no wgpu coverage until both are gone.
Every test folder but `pipeline/` passes alone, as does each file of it but `pixel-semantics-test.cc`.

- **On vulkan, deno's default here, it segfaults.**
  `sg - a sample mask the pixel stage writes keeps the samples it leaves out` crashes it.
  That is [deno-wgpu-sample-mask-crash](../../../../../docs/bugs-external/deno-wgpu-sample-mask-crash/readme.md), outside our code.
- **On dx12 (`DENO_WEBGPU_BACKEND=dx12`) the run dies in JavaScript after about 25 s.**
  `GPUDevice.onuncapturederror` in emdawnwebgpu's handler reads a `message` that is undefined, in `lengthBytesUTF8`.
  So wgpu reports an error whose text does not reach it, and which error that is, and which test causes it, is not known yet.
  Its shape suggests emdawnwebgpu expecting a field deno's error object lacks, unverified.

To close this: name the second error, then decide how the run avoids the first, by choosing dx12 for deno on Windows or by skipping that test where a device cannot take it.
