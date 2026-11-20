---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-viewer/shaders/hybrid_pass.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/hybrid.sgl
  - test: "sv - a frame mesh traces in the frame it is placed, and keeps the image only while its content holds"
closed: 2026-11-20
resolution: fixed
---

# The hybrid routine's light_thin hangs Intel's dx12 device on either shader route

On the Arc B580, driver 32.0.101.9150, `shaped-viewer-test` loses the dx12 device (`DXGI_ERROR_DEVICE_HUNG`) nearly every run, so `dev.py ship` refuses on this machine.
CI runs no Arc and stays green; every other test binary passes here.
The reproducer takes 7 s: `uv run dev.py test "sv - a frame mesh traces in the frame it is placed"`.

This supersedes `bug-sgl-dxil-hybrid-light-hangs-without-load-merging`, whose cause, SGL's native DXIL, no longer holds; its history is the earlier half of this one.

## Measured

- **Not SGL's DXIL.** A context with `dx12_config::native_dxil` off, every shader through DXC, hangs the same way.
  Load merging only moved a threshold.
- DRED, with its newer operations named, shows `light_thin`'s `Dispatch`, then a `Barrier` that never completes.
- **The code only needs to be compiled in, not to run.**
  `bounce_from` returning first behind a frame constant that is never equal still hangs, its trace and glass walk never executed.
- No one function carries it: `hybrid.direct` beside `bsdf_sample_direction`, or beside `passage_of`, passes, while all three hang, and so do `direct`, `bsdf_eval` and `passage_of`.
  It follows the kernel's size: about 47,600 DXIL operations and twelve ray queries.
- Not the private arrays: the seven `float4[28]` environment copies, 3 KB of scratch, are gone with an `@unroll` and it still hangs.
- Not the workgroup: 4 by 4 and 16 by 16 hang as 8 by 8 does.
- `@preferred_subgroup_size(32)` makes it rare rather than gone: four clean runs through DXC, seven then a hang on the native route; 16 hangs at once.
- The AMD RDNA 2 iGPU ran the earlier, unmerged form cleanly.
- Ruled out as triggers: the DXBC checksum of `4c65364c1`, the orthographic camera of `97fe397fe`, and the edge-stop change of `eae3f55a4`.

## Not known

- Whether this is Intel's compiler alone; it is the leading reading, unconfirmed by a standalone DXC reproduction or another driver.

## The fix it points to

Split `light_thin` into two dispatches, the first bounce in one and the follow-up bounce and its lights in the other, so no single kernel reaches this size.

## Fixed by the split, 2026-11-20

`light_thin` is now two kernels per sample: `light_thin` lights the primary point and draws its bounce, `light_thin_bounce` lights the rest.
A record of eight `float4`s and the random stream per pixel passes between them, the image lit one band of at most 2^19 pixels at a time.
Their native DXIL is 19,163 and 26,777 instructions, against 45,602 for the one kernel; max live words 260 and 236, against 319.
- **Measured on the Arc B580, driver 32.0.101.9150**, a downstream viewer's capture in the default mode, 30 runs each in alternating blocks of 10:
  the one kernel lost the device or hung in 24, passed none; the split hung in none and passed 21.
  The other failures of both are a heap corruption of their own, inside Intel's pipeline compile, which the split does not change.
- **The image is unchanged.** Each pixel's random stream is drawn in the same order, and on the AMD iGPU a capture with the one kernel and with the split agree in every pixel.
- This issue's own reproducer no longer reproduced the hang that day, the one kernel included, so it measured nothing either way.
