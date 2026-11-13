---
created: 2026-11-13
refs:
  - test: sv vulkan - every mode and every denoiser draws the view it draws on dx12
---

# Intel's Vulkan compiler crashes the viewer test that draws every mode and denoiser

**The test crashes the whole `shaped-viewer-test` binary with an access violation inside Intel's Vulkan shader compiler.**
Seen 2026-11-13 on an Arc B580, Vulkan driver 1663934 (dx12 driver 32.0.101.9150).

Known:

- The faulting thread is the driver's own: `igc_default64!OpenVulkanCompiler` under `igvk64`, so no frame names the pipeline being compiled.
- It reproduces on every run of this test alone; the other vulkan path-tracer tests, run alone, pass.
- It reproduces at `0507d7ace` and with SGL reverted to it, so it is not from the autodiff commits shipped beside it.
- Since the binary crashes, every case of it is red with it, which `dev.py ship` files as unproven rather than blocking.

Not known: which pipeline it compiles, and whether a cleared driver shader cache or an older driver changes it.
