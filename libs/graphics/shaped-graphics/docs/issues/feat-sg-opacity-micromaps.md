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

The sg API, the vulkan and dx12 backends and the shaped-rendering baker, `sr::micromap_bake_routine`, are in.
The baker reads the UVs on the host to size the build, and a GPU test traces a baked cutout on both backends.
`ray_flags.force_omm_2_state` is in SGL, and a test traces the baked quad both ways on both backends: 4-state exact, 2-state taking the guesses.
Left is sv's integration.
dx12 needs the Agility SDK, and its two GPU tests run on a device of ray-tracing tier 1.2.
**Sponza no longer motivates the rest.**
Its 2.5 ms came from a dirt decal whose opacity is a soft mask, 79% of it strictly between 0 and 1, where an exact micromap decides little.
sv's hybrid now skips such coverage instances through the instance mask instead, which removed the cost.
What remains is thresholded foliage, Bistro's for one, measured before the baker is built.


## sv's integration, the part left

Mapped on 2026-11-05 for whoever takes it; paths are under `libs/graphics/shaped-viewer/`.

**A micromapped BLAS is keyed on mesh and opacity, not on mesh alone.**
`mesh_manager::acquire_blas` (`resources/resource_managers.cc`) builds one BLAS per position-and-index content, shared by every material, and never rebuilds it.
A micromap belongs to a mesh, its opacity texture, its UV set and its cutoff, so a cutout instance needs a BLAS of its own beside the shared one.
It is built late, once its texture lands, and until then the instance traces the shared BLAS and asks its any-hit decision as today.

**The hook is the landed-texture list.**
`texture_manager::collect_settled` feeds `gpu_resource_manager::_collect_textures` and `_queue_mip_work`, which is where a bake can be queued for every cutout instance waiting on that texture.

**The bake needs the CPU UVs and indices, which sv drops.**
The managers free a mesh's staging bytes after the copy; only the caller's `sv::mesh` and `sv::asset_data` keep their pinned payloads.
Either `create_mesh` keeps the UV and index payloads of a mesh a cutout material names, or the baker learns to read them from the GPU buffers.

**Check how forcing an instance non-opaque meets a micromap.**
A cutout instance is built with `opaque_override = false`, which is FORCE_NON_OPAQUE (`rendering/view_renderer.cc`).
If that override wins over the micromap on DXR 1.2 or vulkan, a micromapped instance must drop it and its geometry must be built non-opaque instead.

**The hybrid's rays force 2 states, the others stay 4-state.**
`trace_hit`, `blocked` and `reaches` in `shaders/sgl/tracer_wavefront.sgl` are shared by the hybrid and the wavefront, so they take a flags parameter, or the hybrid gets its own spellings.

**Two gaps in the baker against sv's samplers.**
sv repeats, clamps or mirrors per axis, and the bake knows only clamp or repeat for both.
sv's opacity sampler may be nearest, and the bake assumes a bilinear footprint, which is conservative for nearest but leaves more unknown than needed.

**Done** is still the measurement: Bistro's thresholded foliage, whose candidate stops `tests/hybrid-benchmark.cc` already counts.
