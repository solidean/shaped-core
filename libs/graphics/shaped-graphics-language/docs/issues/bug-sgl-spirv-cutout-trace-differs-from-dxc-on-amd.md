---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-viewer/tests/cross-backend-trace-test.cc
  - test: "sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan"
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-26T13:32Z
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

## Log

2026-11-25: it does not reproduce on RADV (Steam Deck, RDNA 2, Mesa's ACO), now that the test also builds on Linux with vulkan alone.
`--thorough`, every case on the native SPIR-V route agrees with DXC's: cutout differs in 64 of 2304 pixels, by up to 2.8e-7 absolute and 1.5e-6 relative.
So the same SPIR-V traces alike on another RDNA 2 compiler, which points at AMD's own Windows compiler rather than at what SGL writes; the module diff is still the next step.

2026-11-26: worse now, on the AMD iGPU with vulkan driver 2.0.353: the cutout case through SGL's SPIR-V traces an image of nothing, then the device is lost.
It happens on every run under `--thorough`, in under 8 s once the shaders are cached:
`SC_GPU=amd uv run dev.py test "sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan" --thorough`.
The DXIL route agrees with DXC's on every case of the same run, to 1.7e-6 relative.
Not yet known: whether DXC's SPIR-V traces the case on this driver in the same process, and what changed since the 32 pixels, the driver or SGL.
