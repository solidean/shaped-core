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

## Log

2026-11-25: it does not reproduce on RADV (Steam Deck, RDNA 2, Mesa's ACO), now that the test also builds on Linux with vulkan alone.
`--thorough`, every case on the native SPIR-V route agrees with DXC's: cutout differs in 64 of 2304 pixels, by up to 2.8e-7 absolute and 1.5e-6 relative.
So the same SPIR-V traces alike on another RDNA 2 compiler, which points at AMD's own Windows compiler rather than at what SGL writes; the module diff is still the next step.

2026-11-26: worse now, on the AMD iGPU with vulkan driver 2.0.353: the cutout case through SGL's SPIR-V traces an image of nothing, then the device is lost.
It happens on every run under `--thorough`, in under 8 s once the shaders are cached:
`SC_GPU=amd uv run dev.py test "sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan" --thorough`.
The DXIL route agrees with DXC's on every case of the same run, to 1.7e-6 relative.
Not yet known: whether DXC's SPIR-V traces the case on this driver in the same process, and what changed since the 32 pixels, the driver or SGL.

**2026-11-26, later: not SGL's, and not the cutout case alone.**
Traced alone, with `SC_EXP_CASE`-style selection in an experiment, cutout passes on both SPIR-V routes, to 1e-6 relative.
It goes black, and mostly loses the device, only after another case traced in the same process, and only some:

| traced first, then cutout | result on AMD's vulkan |
|---|---|
| nothing, pbr, openpbr constants, cutout | traces |
| unlit, mesh attributes | black on both routes, or the device lost |

Unlit followed by pbr, textured, quadrics or openpbr constants traces, so the trigger is the cutout panel's stochastic any-hit after one of those two.
DXC's SPIR-V fails exactly as SGL's does, once DXC's route traces first, so what SGL writes is not the cause.
The Arc traces the same sequence on vulkan, and the AMD iGPU on dx12.
Every hit stage of the cutout pipeline is byte for byte the same in the run that traces and the run that fails, on both routes.
Not it, each tried:
- the alpha-cutoff box or the quadrics, since the floor, a plain box and the panel suffice;
- the persistent pipeline tier, off under `SC_SG_COLD=pipelines`;
- the previous case's resources being released and their slots reused, since keeping them alive changes nothing;
- the `StorageBufferArrayNonUniformIndexing` capability both routes leave out, patched in.
Validation, synchronization validation and GPU-assisted validation report nothing before the loss.
So one pipeline, from the same modules and the same scene, traces or hangs by what the process ran before, which points at state in AMD's driver.
Next: reduce outside sv, from the dumped modules and the layout they bind, or ask whether the driver caches compiled hit shaders across pipelines.
