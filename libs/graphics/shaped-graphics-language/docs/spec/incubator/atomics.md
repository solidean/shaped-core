# Atomics Beyond the Floor

*Incubator: not normative.*

## The idea

**SGL's atomics are the ones every target has**: 32-bit `uint` and `int`, and a `float` held as a `uint`'s bits, in a `mut buffer` or in workgroup memory, relaxed.
They stand in a pixel, a compute or a ray-tracing stage that runs once per ray or call ([bindings.md](../bindings.md#atomics)).
A vertex, geometry or tessellation stage takes them, and every store through a binding, behind `vertex_pipeline_stores` ([CHK-531](../semantics/checking.md#atomics)).
Image atomics have come back already: an `@atomic` image's texels are atomics under `image_atomics`, with the buffer atomics' methods ([CHK-372](../semantics/checking.md#atomics)).
Each thing below is what one target or another lacks, so each comes back behind an sg feature when a shader wants it, and never as a target's own dialect.

## What each would need

| what | who lacks it | how it would come back |
|---|---|---|
| orderings stronger than relaxed | WGSL, which has relaxed alone | acquire and release as named arguments, once a shader needs orderings rather than the coherence `@coherent` gives |

64-bit atomics in workgroup memory have left it too, as `shader_shared_int64_atomics` (CHK-510).
So have a vertex, geometry or tessellation stage's atomics, and its stores with them, as `vertex_pipeline_stores` (CHK-531).
The compare-exchange has left the incubator, as a strong and a weak form ([bindings.md](../bindings.md#atomics), [EMIT-161](../semantics/emitting.md#types)).

## Open

* Whether HLSL's `Interlocked*` without an out value, for an update whose result is dropped, is worth a writer of its own.
