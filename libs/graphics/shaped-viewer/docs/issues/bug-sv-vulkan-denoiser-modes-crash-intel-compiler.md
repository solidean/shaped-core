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

## Log

**2026-11-26: the first crash is a ray-tracing pipeline's empty hit groups, and sg's vulkan backend fills them on Intel.**
The faulting create is the megakernel's: a raygen, two misses, and ten hit groups alternating a closest hit with an empty one.
Serializing every ray-tracing create changed nothing, and neither did leaving its pipeline cache out.
Trial creates of subsets of it, on the Arc B580 with vulkan driver 1663934:

| hit groups | result |
|---|---|
| empty groups alone, up to 12 | builds |
| one closest hit in each of 9 groups | builds |
| 8 groups of the 5 closest hits, none empty | builds |
| the 5 closest hits, then one empty group | builds |
| one closest hit, then 6 empty groups | builds |
| one closest hit, then 7 empty groups | crashes |
| 4 pairs of a closest hit and an empty group | crashes |
| the whole pipeline, its empty groups given a closest hit that returns | builds |

A trivial closest hit in place of the viewer's builds every one of those shapes, so the viewer's large stage is part of the trigger, and no standalone repro exists yet.
`vulkan_raytracing_pipeline.cc` now gives an empty hit group a closest hit that returns at once on an Intel GPU, which leaves the payload as none would.
With it the test's megakernel and wavefront agree with dx12 on the Arc.

Still open, so the test still skips Intel: the forward hybrid's depth guide reads 20 non-finite pixels on every frame.
After that the process dies of heap corruption, again inside `igc_default64` but on a thread of the driver's own, so no frame names the pipeline.
