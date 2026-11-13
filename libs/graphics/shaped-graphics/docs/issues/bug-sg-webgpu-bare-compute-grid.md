---
created: 2026-11-11
refs:
  - test: "sg - a bare SGL @compute runs the threads dispatch_threads asks for alone*"
  - test: "sg - a package shader that asserts is acquired and runs*"
  - test: "sg - an SGL affine3 in GPU memory is its three rows*"
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_command_list.compute.cc
  - symbol: webgpu_command_list::compute_dispatch
---

# A bare SGL @compute on webgpu reads a grid of zero, so every thread returns

On the `emscripten-webgpu-relwithdebinfo` preset under node, a bare `@compute` writes nothing: every cell of the dispatch test stays -1.
The three tests above fail; on dx12 and vulkan they pass.

Two layers, found while landing the mesh polyfill (2026-11-11):

- **At `main` before it, the shader did not compile at all.**
  slib's WGSL declaration reader refused `sg_dispatch_grid` at `@group(3) @binding(21)` as no binding the reserved group holds.
  The mesh polyfill made it read every entry of sg's reserved table by name and address, which this compile now gets past.
- **Past that, the threads read a grid of zero**, so CHK-520's guard returns them all.
  `compute_dispatch` writes the grid into the reserved block where the bound layout `has_dispatch_grid()`.
  Whether the block reaches the shader at `grid_offset_in_block`, with its dynamic offset in binding order, is unchecked.

The ship gate runs the native preset alone, which is how this went unseen; the webgpu suite also has unrelated failures of its own (sgl lsp layers, an rgb10a2 storage image in sr's mix, shaped-xr).
