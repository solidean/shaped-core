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

The compare-exchange has left the incubator, as a strong and a weak form ([bindings.md](../bindings.md#atomics), [EMIT-157](../semantics/emitting.md#types)).

## Open

* Whether a ray-tracing stage takes atomics, which every target would allow, beside the pixel and compute stages.
* Whether HLSL's `Interlocked*` without an out value, for an update whose result is dropped, is worth a writer of its own.
