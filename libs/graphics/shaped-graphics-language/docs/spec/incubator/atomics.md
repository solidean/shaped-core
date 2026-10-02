# Atomics Beyond the Floor

*Incubator: not normative.*

## The idea

**SGL's atomics are the ones every target has**: 32-bit `uint` and `int`, in a `mut buffer` or in workgroup memory, relaxed, in a pixel or a compute stage ([bindings.md](../bindings.md#atomics)).
Image atomics have come back already: an `@atomic` image's texels are atomics under `image_atomics`, with the buffer atomics' methods ([CHK-372](../semantics/checking.md#atomics)).
Each thing below is what one target or another lacks, so each comes back behind an sg feature when a shader wants it, and never as a target's own dialect.

## What each would need

| what | who lacks it | how it would come back |
|---|---|---|
| float atomics, `atomic[float].add` | WGSL; vulkan needs `shaderBufferFloat32AtomicAdd` | an sg feature, refused by the check pass where it is not granted |
| 64-bit atomics | WGSL | an sg feature over `atomic[long]` and `atomic[ulong]`, once SGL has 64-bit integers, named as `short` and `ushort` are |
| a vertex stage's atomics and stores | WebGPU; vulkan needs `vertexPipelineStoresAndAtomics` | `vertex_stores`, lifting `@stages(.pixel, .compute)` from the builtins |
| orderings stronger than relaxed | WGSL, which has relaxed alone | acquire and release as named arguments, once a shader needs orderings rather than the coherence `@coherent` gives |

## A compare-exchange

**It is left out of the floor for its result, not for want of a target.**
Every target has one, and WGSL's is weak: it may fail although the value matched, so the result has to say whether it exchanged, not only what it read.
WGSL returns that as a struct whose type no program can name, and HLSL gives the value before through an out parameter, so `exchange_result[T]` needs a spelling in each.
A helper struct written ahead of the entry point, filled from the target's own result, is the likely shape; `exchanged` must come from the target and never from comparing values.

## Open

* Whether a compare-exchange returns a struct, or takes the expected value and gives a `bool` with the value before in a named argument.
* Whether HLSL's `Interlocked*` without an out value, for an update whose result is dropped, is worth a writer of its own.
