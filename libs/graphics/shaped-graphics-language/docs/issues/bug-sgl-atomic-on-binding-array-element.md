---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_arrays.cc
  - symbol: sgl::check::checker::resolve_array
---

# An atomic update of a binding array's element reads as an array of arrays

An atomic method on an element of a binding array of atomic buffers is refused with CHK-285's diagnostic, which is about type syntax:

```sgl
require binding_arrays

binding heap:
    roi: mut buffer[atomic[uint]][4]

@compute(64) fun cs(@thread_id id: int3){heap}:
    let b = heap.roi[1][0].min(5u)
```

`error: wrong-kind-of-name: an array of arrays is one group of dimensions, outermost first: float[3, 5]`, at `heap.roi[1][0]`.

What is known:

- A plain write through the same path, `heap.roi[i][0] = 5u` on a `mut buffer[uint][4]`, compiles on dx12.
- An atomic on a buffer that is no binding array, `heap.roi[0].min(5u)`, compiles on dx12 and WebGPU.
- The index needs no uniformity to fail: a constant `1` fails as `args.i` does.
- `binding_arrays` is already `target-lacks-feature` on WebGPU, so WebGPU's atomic model asks nothing new here.
- Hypothesis: the method call probes its receiver `a[b][c]` as a type, and `resolve_array` reports instead of declining.

Wanted: the call compiles on dx12, vulkan and metal, as `InterlockedMin` on `heap_roi[1][0]` and its equivalents.
Reported from a port where it blocks three shaders.
