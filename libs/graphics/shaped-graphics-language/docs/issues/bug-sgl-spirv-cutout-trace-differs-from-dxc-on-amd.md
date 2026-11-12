---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-viewer/tests/cross-backend-trace-test.cc
  - test: "sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan"
---

# On AMD's vulkan driver, the viewer's cutout trace through SGL's SPIR-V differs from DXC's in 32 pixels

On the AMD Radeon iGPU (RDNA 2, `SC_GPU=amd`), `held_to_dxc` in `cross-backend-trace-test.cc` fails for the `cutout` case on vulkan.
The image through SGL's own SPIR-V differs from DXC's in 32 of 2304 pixels, by up to 0.29 absolute, 1.0 relative; the tolerance is 1e-4.
Every other case agrees on vulkan, every case agrees on dx12, and the Intel Arc B580 agrees on both.
`sv vulkan - the path tracer traces what it traces on dx12, to rounding` fails on `cutout` alone as well: 164 of 2304 pixels, up to 0.08 absolute.
Both routes default to SGL's binary there, so AMD's vulkan `cutout` disagrees with its dx12 one too.

`cutout` is the case whose any-hit decides alpha against a texture's cutoff, so 32 pixels flipping whole reads as hits accepted on one route and ignored on the other.
Likely suspects are what differs between the two modules there: the LOD the any-hit samples at, the comparison's float flags, or how `ignore_hit` lowers.
`native-parity.py diff` on the viewer's cutout any-hit is the first step; nothing has been compared yet.
This is SGL's to fix or to explain, since both routes run one source on one driver.
