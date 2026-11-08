---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_binding_group.cc
  - test: "sg - a staging group over an unbounded binding array holds its capacity, set and replaced in place"
closed: 2026-11-08
resolution: fixed
---

# On AMD's vulkan driver, a staging group over an unbounded binding array writes a descriptor image larger than its layout

On the AMD Radeon iGPU (RDNA 2, `SC_GPU=amd`), the test fails on the vulkan native-spirv context with `vulkan_binding_group::create_from_image`'s assert.
The message is "the descriptor image is one set, its unbounded binding cut short": `image.size() <= layout->_size_in_bytes` does not hold.
The run then logs an undeclared "command list destroyed without submit or drop", which is the failed test's unwinding.
It passes on the Intel Arc B580 on every context.
`sg - an unbounded array binding is its layout's last, and each group sizes it by its views` fails the same assert on AMD, on both vulkan contexts, so it is not the route.

Not yet known: the two sizes, and whether the vulkan context on the DXC route fails too (it ran in another shard and passed there, or did not reach the test).
The likely cause is the layout's size for an unbounded binding being taken from a capacity or a descriptor size that AMD reports differently.
AMD's descriptor-buffer sizes and alignments differ from Intel's, so that arithmetic is the first place to look.

## Cause

AMD's driver (vulkan 2.0.353) sizes a layout whose last binding is `VARIABLE_DESCRIPTOR_COUNT` as though that binding held no elements.
`vkGetDescriptorSetLayoutSizeEXT` gave 16 bytes for one storage buffer followed by 65,536 more at offset 16, where the spec says the size counts the variable binding at its full count.
Intel's driver follows the spec.
`vulkan_binding_group_layout::create` now takes the larger of the driver's size and the last binding's offset plus its capacity, which is what the spec's own size is.
