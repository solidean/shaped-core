# Anisotropic LOD

One `sg::sampler` picks the same mip level on every backend, with or without anisotropy and whatever its `mip_lod_bias`.
That level is the one dx12 picks, which is also what the specifications describe.
It does not hold of the drivers on their own, and this is where sg makes it hold.

## What the drivers do

NVIDIA's drivers sharpen the trilinear blend once anisotropy is on, on dx12 and vulkan alike.
The weight of the coarser mip rises over less than a level, with a slope s that depends on the sampler's anisotropy:

| max_anisotropy | 1 | 2 - 3 | 4 - 7 | 8 - 16 |
|---|---|---|---|---|
| blend slope s | 1 (linear) | 1.5 | 1.75 | 2 |
| how much blurrier vulkan is than dx12, (1 - 1/s) / 2 | 0 | 1/6 | 3/14 | 1/4 |

The slopes are vulkan's, and its shift follows them.
dx12 differs at 3x alone, sharpening it like 4x to 1.75, which moves nothing since dx12 does not shift.

**dx12 centres the sharpened ramp on the half level, and vulkan starts it at the integer.**
So vulkan reaches the coarser mip (1 - 1/s) / 2 levels early and samples as if the level were that much higher, a quarter of a level at 8x and above.

The shift does not depend on the bias, on the footprint, or on how the level was reached.
The implicit level, `sample(…, bias =)`, explicit gradients and an explicit `level` all shift alike, an isotropic footprint as much as a 16:1 one.
It is therefore not a difference in level selection at all, which is why no shader-side correction is needed.

Measured on an RTX 5080 with driver 610.88 on Windows.
AMD's drivers on a Radeon iGPU show no shift: both cross the half level where the specification does.
Their blends still differ in shape, AMD's vulkan sharpening around the half level and its dx12 staying linear, which no bias can undo.
Apple's Metal driver on an M4 shows no shift and keeps the blend linear at every anisotropy.
Other NVIDIA generations, drivers and operating systems are assumed to behave the same until measured otherwise.
The driver's own texture-filtering settings (the NVIDIA control panel's trilinear and anisotropic optimizations) may change both curves.

A max_anisotropy of 3 is not applied as 3 once the footprint is more anisotropic than that.
NVIDIA and AMD apply it as 2 on both APIs, so a 4:1 footprint reads log2(3/2) levels coarser than specified; the M4 applies it as 4, reading log2(3/4) finer.
sg does not unify this yet, and 6x and 12x are unmeasured.

## What sg does

The vulkan backend adds `sg::backend::vulkan::anisotropic_lod_correction` to every anisotropic sampler's `mipLodBias`: -(1 - 1/s) / 2 on NVIDIA, 0 elsewhere.
Shifting the level by that much moves vulkan's ramp onto dx12's exactly, so the two blends coincide everywhere rather than only at the half level.
Every path takes the sampler's bias, so one correction in `to_vk_sampler_info` covers them all: dynamic samplers, static samplers and SGL's declared ones.

`vulkan_config::correct_anisotropic_lod` turns it off, which is for measuring the driver and nothing else.

## Measuring it

`libs/graphics/shaped-rendering/tools/lod-probe` measures this on every GPU of a machine, through both backends, and is meant to be handed to others.
It reads the level where the sample crosses each half level, which any symmetric blend keeps, and reports the blend's width beside it.
Its `--images` figures show the gap on a textured floor.
A new driver behaving differently shows up there first, as a nonzero `dx12 minus vulkan+sg` row.

`sg - an anisotropic sampler is half way between two mips at the half level, its bias included` is the regression test.
It samples an explicit half level through anisotropic samplers on every backend, which reads half a level high on NVIDIA's vulkan without the correction.
