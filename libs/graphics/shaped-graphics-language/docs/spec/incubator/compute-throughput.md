# Compute throughput: half precision and matrix fragments

## The idea

A compute-bound kernel written in SGL can tile through workgroup memory today, and still cannot reach the throughput its hardware has.
The case that brought it here is shaped-rendering's OIDN member: a U-Net of sixteen 3x3 convolutions, run in HLSL at about 3.2 TFLOP/s against a 20-25 fp32 peak.
Its route to speed is [denoising.md's "Getting faster"](../../../../shaped-rendering/docs/denoising.md#getting-faster).
The first step, implicit-GEMM tiling, needs only the [`@workgroup` bindings and barriers](../bindings.md#workgroup-memory) SGL already has.
The steps after it ask two things of SGL, in order of what they buy for the portability they cost:

* **A 16-bit float type**, as a [feature](../semantics/checking.md#features) a file or body `require`s.
  It halves bandwidth and workgroup-memory footprint, which buys larger tiles; packed arithmetic doubles ALU rate on some GPUs.
  DX12 has it with SM 6.2 and `-enable-16bit-types`, Vulkan with `VK_KHR_shader_float16_int8` and `VK_KHR_16bit_storage`, Metal outright, and WebGPU as the optional `shader-f16`.
* **Matrix fragments**, as a feature too, with a non-matrix fallback that stays mandatory.
  Vulkan has `VK_KHR_cooperative_matrix` and Metal `simdgroup_matrix`.
  DirectX's replacement for SM 6.9's withdrawn cooperative vectors is SM 6.10's linear-algebra matrices, still in preview, and WebGPU's subgroup matrices are Dawn-experimental.
  This is what closes the last ~3x between a well-tuned fp32 kernel and OIDN's own tensor-core path.

## What it touches

* The type system: an `f16` scalar and its vectors, and a matrix-fragment type whose shape is a compile-time value.
* [Features](../semantics/checking.md#features): `f16` and matrix fragments are two more a device may lack, each granted by `require`.
* [uniformity.md](uniformity.md): a matrix operation is a subgroup-wide one, so it has the same uniformity question a barrier does.

## Already fixed by the syntax

`require` is how a non-portable feature is opted into, so neither addition needs a new mechanism for that.

## Open

* How a matrix fragment's per-backend shape limits are expressed, since they differ by vendor and generation.
* Whether `f16` arithmetic and `f16` storage are one feature or two, since some devices offer storage alone.
