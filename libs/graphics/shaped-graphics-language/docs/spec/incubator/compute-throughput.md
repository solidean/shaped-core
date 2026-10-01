# Compute throughput: matrix fragments

## The idea

A compute-bound kernel written in SGL can tile through workgroup memory today, and still cannot reach the throughput its hardware has.
The case that brought it here is shaped-rendering's OIDN member: a U-Net of sixteen 3x3 convolutions, run in HLSL at about 3.2 TFLOP/s against a 20-25 fp32 peak.
Its route to speed is [reconstruction.md's "Getting faster"](../../../../shaped-rendering/docs/reconstruction.md#getting-faster).
The first step, implicit-GEMM tiling, needs only the [`@workgroup` bindings and barriers](../bindings.md#workgroup-memory) SGL already has.
The steps after it asked two things of SGL.
The first is decided: **a 16-bit float**, `half` and its vectors under the feature `shader_f16` ([CHK-346](../semantics/checking.md#types)).
It halves bandwidth and workgroup-memory footprint, which buys larger tiles, and packed arithmetic doubles the ALU rate on some GPUs.
The second is still this file's idea:

* **Matrix fragments**, as a feature, with a non-matrix fallback that stays mandatory.
  Vulkan has `VK_KHR_cooperative_matrix` and Metal `simdgroup_matrix`.
  DirectX's replacement for SM 6.9's withdrawn cooperative vectors is SM 6.10's linear-algebra matrices, still in preview, and WebGPU's subgroup matrices are Dawn-experimental.
  This is what closes the last ~3x between a well-tuned fp32 kernel and OIDN's own tensor-core path.

## What it touches

* The type system: a matrix-fragment type whose shape is a compile-time value, over `half` or `float`.
* [Features](../semantics/checking.md#features): matrix fragments are one more a device may lack, granted by `require`.
* [Subgroups](../semantics/checking.md#subgroups): a matrix operation is a subgroup-wide one, so it stands where a subgroup operation does, in uniform control flow.

## Already fixed by the syntax

`require` is how a non-portable feature is opted into, so neither addition needs a new mechanism for that.

## Open

* How a matrix fragment's per-backend shape limits are expressed, since they differ by vendor and generation.
