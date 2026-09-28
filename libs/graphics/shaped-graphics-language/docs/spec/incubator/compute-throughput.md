# Compute throughput: workgroup memory, half precision and matrix fragments

## The idea

A compute-bound kernel written in SGL today cannot reach the throughput its hardware has, because three things every fast compute kernel uses are missing.
The case that brought it here is shaped-rendering's OIDN member: a U-Net of sixteen 3x3 convolutions, run in HLSL at about 3.2 TFLOP/s against a 20-25 fp32 peak.
Its route to speed is [denoising.md's "Getting faster"](../../../../shaped-rendering/docs/denoising.md#getting-faster), and each step past the first asks one of these of SGL.

In order of what they buy for the portability they cost:

* **Workgroup memory and a workgroup barrier.**
  A workgroup stages an input halo and a slab of weights once, and each thread computes a block of outputs from them.
  That is implicit-GEMM tiling, which is how every fast portable convolution works, and it is available on dx12, vulkan, metal and webgpu alike.
* **A 16-bit float type**, behind a feature level.
  It halves bandwidth and workgroup-memory footprint, which buys larger tiles; packed arithmetic doubles ALU rate on some GPUs.
  DX12 has it with SM 6.2 and `-enable-16bit-types`, Vulkan with `VK_KHR_shader_float16_int8` and `VK_KHR_16bit_storage`, Metal outright, and WebGPU as the optional `shader-f16`.
* **Matrix fragments**, behind a feature level, with a non-matrix fallback that stays mandatory.
  Vulkan has `VK_KHR_cooperative_matrix` and Metal `simdgroup_matrix`.
  DirectX's replacement for SM 6.9's withdrawn cooperative vectors is SM 6.10's linear-algebra matrices, still in preview, and WebGPU's subgroup matrices are Dawn-experimental.
  This is what closes the last ~3x between a well-tuned fp32 kernel and OIDN's own tensor-core path.

## What it touches

* The type system: an `f16` scalar and its vectors, and a matrix-fragment type whose shape is a compile-time value.
* [feature-levels.md](feature-levels.md): `f16` and matrix fragments are the first features a real kernel would declare.
* Stage interfaces: workgroup-memory declarations belong to a compute entry point, and a barrier is only meaningful inside one.
* [uniformity.md](uniformity.md): a barrier in divergent control flow is the compute analogue of a derivative there.

## Already fixed by the syntax

Nothing yet.

## Open

* Whether workgroup memory is a declaration at module scope, as HLSL's `groupshared`, or a parameter of the entry point.
* How a matrix fragment's per-backend shape limits are expressed, since they differ by vendor and generation.
* Whether `f16` arithmetic and `f16` storage are one feature or two, since some devices offer storage alone.
