---
created: 2026-11-08
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/lower_memory.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/hlsl.cc
  - test: "sr - the length of a binding array's element at a non-uniform index is that element's, in a fixed array and an unbounded one"
---

# On vulkan, a binding array element's length at a non-uniform index is asked at uniform indices, since AMD's driver answers it from the first lane

On the AMD Radeon iGPU (RDNA 2, vulkan driver 2.0.353, `SC_GPU=amd`), `OpArrayLength` misreads a binding array's element at a non-uniform index.
Every lane gets the length of the element the first active lane indexes.
The `NonUniform` decorations are on the index and the access chain, and `spirv-val` accepts the module.
The load through the same access chain reads the right element, and dx12 on the same GPU answers each lane its own length.
DXC's SPIR-V and SGL's own fail alike, and so did every guard of an index into such an element under `report` and `define`.
Measured: a view of 2 and a view of 8, read at `nonuniform (id.x % 2)`, gave `2 2 2 2` where `2 8 2 8` is right.

**The workaround, on vulkan only:** SGL asks each length at a uniform index, in the IR lowering (`uniform_element_length`) and in the HLSL-for-vulkan length helper alike.
- A fixed array of up to 16 elements asks every element and selects the lane's, which needs no subgroup operation and runs in every stage.
- An unbounded array, or a larger one, takes a waterfall over `broadcast_first` / `WaveReadLaneFirst`, in the compute and pixel stages alone, where sg requires subgroup operations.
  In every other stage it is still asked as it is, and still wrong on AMD.

It reproduces standalone, with descriptor sets as well as descriptor buffers:
[amd-vulkan-nonuniform-array-length-reads-first-lane](../../../../../docs/bugs-external/amd-vulkan-nonuniform-array-length-reads-first-lane/readme.md).

**Remove it** once that entry's `run.py --vendor 1002` exits 0 on a newer AMD driver.
