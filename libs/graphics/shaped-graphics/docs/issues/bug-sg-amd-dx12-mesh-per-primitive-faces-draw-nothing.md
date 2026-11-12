---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/tests/raster/mixed-route-test.cc
  - test: "sg - mesh and task stages of SGL's binary link against DXC's in every pairing and draw alike"
---

# On AMD's dx12 driver, the mesh fixture's per-primitive faces draw nothing, through DXC too

On the AMD Radeon iGPU (RDNA 2, driver 32.0.21042.62, `SC_GPU=amd`), the dx12 hardware context draws nothing for the `faces` pair of `mesh_stages`.
That covers `faces_ms` with `faced_ps`: the reference, every stage through DXC, covers no pixel (`covered > 0` fails), and the three per-face color checks read 0.
The `quad` pair draws on the same context, so mesh stages work and the per-primitive outputs `faces_ms` writes are the difference.
AMD's vulkan context passes the test, and so does Intel's dx12.

So it is not the native route: the all-DXC reference fails.
Not yet known: whether AMD drops a mesh shader's per-primitive attributes, or whether sg's dx12 mesh pipeline describes them in a way AMD reads differently.
A capture with the debug layer's GPU validation, or a standalone mesh shader writing one primitive attribute, would tell.
