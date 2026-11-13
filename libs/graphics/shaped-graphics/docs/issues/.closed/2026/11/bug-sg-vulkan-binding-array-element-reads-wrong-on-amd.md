---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-rendering/tests/debug-output-test.cc
  - test: "sr - define runs a load of more words than the memory holds on none of them, and gives zero (LEGAL-68)"
closed: 2026-11-08
resolution: fixed
---

# On AMD's vulkan driver, a binding array's elements read the wrong buffer or nothing, on both routes

On the AMD Radeon iGPU (RDNA 2, `SC_GPU=amd`), four of `shaped-rendering-test`'s guard tests fail on both vulkan contexts, DXC's route and SGL's own SPIR-V alike.
They are two tests, each on both contexts:

- `sr - an index past an element of a binding array is measured against that element: reported, or under define the element's first (LEGAL-66, LEGAL-68)`
- `sr - define runs a load of more words than the memory holds on none of them, and gives zero (LEGAL-68)`
Their dx12 contexts pass, and so do all of them on the Intel Arc B580.

Measured in `debug-output-test.cc`:

- The element test expects `{11, 22, third, 24}` and reads `data[1]` and `data[3]` as 99 under `report`, which is the buffer's initial contents, so those lanes wrote nothing.
- Under `define` the same lanes read 20 where 22 and 24 are right, so they read another element's data.
- The load test expects 42 in `data[1]` and `data[3]` and reads 0.

Every failing lane reads through a binding array's second element, so the second element's descriptor is likely the wrong one or absent.
That is also where [the unbounded layout assert](bug-sg-vulkan-unbounded-staging-group-outgrows-layout-on-amd.md) points, so the two probably share one cause.
That cause is sg's descriptor-buffer arithmetic for array elements, a stride or an offset that AMD's descriptor sizes break and Intel's happen not to.

## Cause

It was not the descriptors: the layout was right, offsets 0 and 16 in 48 bytes, and every load reached the element its lane indexed.
The guards read the wrong length.
AMD's driver answers `OpArrayLength` of an element at a non-uniform index with the first active lane's element, so lanes 1 and 3 measured their index against element 0's length.
SGL now asks that length at uniform indices on vulkan.
[debt-sgl-vulkan-element-length-asked-at-uniform-indices](../../../../../../shaped-graphics-language/docs/issues/debt-sgl-vulkan-element-length-asked-at-uniform-indices.md) tracks the workaround.
