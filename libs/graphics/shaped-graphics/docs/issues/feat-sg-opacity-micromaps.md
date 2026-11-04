---
created: 2026-10-17
parent: epic-sg-acceleration-structure-gaps
---

# Opacity micromaps

Behind an sg feature: per-micro-triangle opaque / transparent / unknown masks built with the BLAS.
Most candidate hits on alpha-tested geometry then never reach an any-hit decision.
DXR 1.2 and vulkan's `VK_EXT_opacity_micromap` expose them to ray queries too.
The planned caller is shaped-viewer's wavefront tracer, which keeps cutout opacity as data so it can bake them — [wavefront.md](../../../shaped-viewer/docs/wavefront.md).
The hybrid renderer is a second caller, measured on 2026-10-28: on Sponza its inline rays spend 2.5 ms of a 5.1 ms lighting pass deciding candidates on one BLEND material.
Forcing every instance opaque recovers all of it, and sampling the opacity at a coarser mip none, so the cost is the decisions themselves.
[hybrid-renderer.md](../../../shaped-viewer/docs/plans/hybrid-renderer.md#what-a-whole-frame-measured) has the measurement.

## The design

Settled in a design review on 2026-10-29.

**Both dx12 and vulkan, and dx12's micromaps come with the D3D12 Agility SDK.**
dx12 has micromaps only from DXR 1.2, which ships in the Agility SDK 1.619 or later with Shader Model 6.9, not in the runtime Windows carries.
The SDK became an optional dx12 dependency on 2026-11-04 rather than the required one this review settled: without it dx12 reports no micromaps.
It is fetched on request with `--accept-license`, and the backend loads it through a device factory rather than exports from every executable — [backends.md](../concepts/backends.md#the-dx12-runtime).
vulkan has `VK_EXT_opacity_micromap`, in the installed SDK and the NVIDIA driver; metal has no equivalent we found, and webgpu none.

**A shader opts in through its target, not its source.**
On dx12 SGL adds `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` to every HLSL ray query, which needs Shader Model 6.9, so slib compiles dx12 packages at 6.9.
A ray-tracing pipeline takes the matching pipeline flag.
vulkan needs nothing: there a micromap applies to every query.

**A micromap is its own object, as in both APIs.**
`cmd.raytracing.build_micromap(sg::micromap_triangles)` returns a persistent `sg::micromap_handle`, from a buffer of packed states and one `{offset, level, format}` descriptor per map.
`sg::blas_triangles` gains `micromap` and `micromap_indices`, one index per triangle or one of the four special values for a whole triangle.
The BLAS holds the micromap, the ownership edge a TLAS has with its BLASes.
**Strict where the feature is absent**: `micromap` must be null without `sg::feature::opacity_micromap`, so a caller checks before baking; relaxing that later breaks no one.

**Baked 4-state, and each ray chooses.**
The baker writes 4-state with a guess on every unknown micro-triangle: its center's opacity against the cutoff, or against 0.5 for coverage.
SGL gains the force-2-state ray flag (`RAY_FLAG_FORCE_OMM_2_STATE`, `gl_RayFlagsForceOpacityMicromap2StateEXT`).
The hybrid's rays set it and never stop, since none of them is a primary ray; the wavefront's and the megakernel's keep 4-state and stay exact.
A test traces a cutout quad both ways, pinning 4-state exactness and bounding the 2-state difference.

**The baker is ours, a GPU routine in shaped-rendering.**
It takes UVs, indices, a texture view and channel, a scale and bias, and a cutoff or coverage rule, and writes the inputs of `build_micromap`.
Classification is conservative, against a min/max mip pyramid of the opacity channel that covers what the trace's filter blends in, at the mip the trace samples.
A triangle's level comes from its area in texels, about one texel per micro-triangle, capped by measurement.
NVIDIA's Opacity Micro-Map SDK was the alternative; its GPU path would have to be bridged to sg, and the classification has to follow our sampling rules exactly.

**shaped-viewer bakes once a mesh's opacity texture is resident**, and rebuilds that BLAS once with the micromap.
Until then the shader decides every candidate, as today.
One BLAS per mesh and cutout material where two instances differ; blob-cache only once a measurement shows the bake is worth skipping.

**Done means** a thresholded cutout's candidate stops are gone from the hybrid's lighting pass, measured on a scene whose foliage is alpha-tested.

## Status

The sg API and the vulkan backend are in; the shaped-rendering baker, sv's integration and dx12 are not.
**Sponza no longer motivates the rest.**
Its 2.5 ms came from a dirt decal whose opacity is a soft mask, 79% of it strictly between 0 and 1, where an exact micromap decides little.
sv's hybrid now skips such coverage instances through the instance mask instead, which removed the cost.
What remains is thresholded foliage, Bistro's for one, measured before the baker is built.

