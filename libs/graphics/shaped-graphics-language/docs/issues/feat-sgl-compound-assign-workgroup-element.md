---
created: 2026-10-30
corpus:
  - bgfx/shaders/37-gpudrivenrendering/cs_gdr_stream_compaction
---

# `op=` on an element of a `@workgroup` array

`scan.temp[i] += scan.temp[0]` on a `@workgroup` array is `unsupported-yet: main: its body reaches a construct the flat tree cannot hold yet`.
The same `+=` on a buffer element compiles, and so does `scan.temp[i] = scan.temp[i] + scan.temp[0]`.

```sgl
@workgroup binding scan:
    temp: float[64]

@compute(64) fun main(@local_thread_index i: int){scan}:
    scan.temp[i] = 1.0
    workgroup_barrier()
    scan.temp[i] += scan.temp[0]
```

- **What needs it** is a parallel prefix sum, the core of stream compaction: bgfx's `cs_gdr_stream_compaction` does `temp[bi] += temp[ai]` in its up-sweep.
- The diagnostic names no construct and points at the entry point, which `bug-sgl-flat-tree-unsupported-names-nothing` is about.

`corpus:` names the shader in shader-rosetta (`uv run dev.py companions init shader-rosetta`), under `sources/`.
