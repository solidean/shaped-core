# The hybrid renderer

**A plan, nothing built yet.**
It is the design of sv's interactive mode: rasterized primary visibility, lit by ray tracing over few bounces, with a radiance cache standing in for the rest, then denoised and upscaled.
**Performance is its first requirement**: large scenes, mostly geometry, have to stay interactive, and the image has to look good while they do.
A final render keeps converging with the wavefront tracer and no cache, so nothing approximated here reaches a finished image.

This plan replaces the hybrid's earlier shape in [the roadmap](rendering-roadmap.md): a visibility buffer whose material is re-evaluated per permutation, lit by the megakernel.

## Decisions

Settled by the user on 2026-10-27:

1. **Performance comes first in the interactive mode.**
   Where a choice trades exactness for frame time, this mode takes the frame time and the converging wavefront keeps the exactness.
2. **No ReSTIR, in any form, until NVIDIA answers on its patents** (US 11,922,556 B2 among them).
   The radiance cache comes first.
3. **The radiance cache is an open design no patent covers**, built from published work ([The radiance cache](#the-radiance-cache)).
4. **Primary visibility is rasterized after a depth prepass**, with no visibility buffer.
   Changed on 2026-10-28 from no prepass: the prepass is what lets a pixel stage run once per visible pixel.
5. **The lighting reads a reduced closure from the G-buffer**, not the whole of OpenPBR.
6. **A secondary hit sees a simplified material**, read from data rather than generated code; judged by images once it draws.
7. **Raster culls on the GPU**, and that is a requirement rather than an optimization.
8. **Every draw is indexed, from one vertex buffer and one index buffer that sv manages centrally.**
9. **LODs come later**, as more index ranges in the same buffers.
10. **The TLAS stays rebuilt every frame**, which is cheap enough; sg still grows refit and compaction, for the dynamic parts.
11. **webgpu is out of scope**, as it is for the wavefront.
12. **The target is 60 fps on a laptop RTX 5070**, upscaled: rendering at most at 1080p, output up to 4K, with resolutions mixed where that pays ([Budget](#budget)).
13. **One vertex format, not interleaved**: fixed streams of one layout each ([One vertex buffer, one index buffer](#one-vertex-buffer-one-index-buffer)).
14. **The lighting is built twice, and measured against itself**, decided on 2026-10-28.
    Forward runs it in the material's own pixel stage, tracing inline with the full closure, so nothing but the guides is stored.
    Deferred runs it in a compute kernel over the thin G-buffer, with the reduced closure of decision 5.
    [Forward or deferred](#forward-or-deferred) is the comparison.

## The frame

```
cull ─▶ raster ─▶ light ─────────────▶ denoise + upscale ─▶ composite
(GPU)   (thin      (compute, inline     (dlss_rr / MetalFX /
         G-buffer)  traces; reads and    FSR + our denoiser)
                    feeds the cache)
```

- **cull** decides which instances draw, and bins them by material permutation into indirect draw arguments.
- **raster** draws them, evaluates each pixel's material once, and writes the thin G-buffer and the surface's own emission.
- **light** is one compute dispatch over the pixels at the upscaler's input extent: direct light with a shadow ray, one indirect ray, and the cache past it.
  It writes the noisy radiance and the denoiser's guides.
- **The cache** is updated from the same frame's rays and a sparse set of longer paths ([The radiance cache](#the-radiance-cache)).
- **denoise + upscale** is `sr::reconstruct_routine`, as for a traced layer.

`render_mode::hybrid` is the fourth value of `render_settings::mode`, as [raster.md](../raster.md#selecting-it) anticipated.

## Geometry

### One vertex buffer, one index buffer

**sv sub-allocates every mesh's vertices and indices out of two central buffers**, owned by the mesh manager.
Today each mesh lives in bindless tables, and raster pulls three corners per triangle with no index buffer bound ([raster.md](../raster.md#what-a-frame-is)).

- **A draw is `draw_indexed` with a base vertex and a first index into the central buffers**, so the post-transform cache reuses vertices.
- **A BLAS is built from ranges of the same buffers**, so a mesh is stored once for raster and trace alike.
- **Growth and fragmentation are the manager's**: a buffer that runs out is reallocated larger and copied on the GPU, and freed ranges are reused.

**The vertex buffer is a set of streams, one layout each, sharing one vertex range per mesh** (proposed).
Phase 1 lands the position stream and the index buffer alone.
The other streams wait, because sv's normals, tangents and uvs are named material attributes, numbered per vertex, per corner or per triangle.
A fixed stream changes how a material resolves them, so it is its own step, taken once attribute fetch measures as worth it.

| stream | format | bytes | read by |
|---|---|---|---|
| position | `float3`, mesh-local | 12 | raster, culling, BLAS builds |
| normal | octahedral, two `snorm16` | 4 | the pixel stage, hits |
| tangent | octahedral, two `snorm16`, its sign in the low bit | 4 | normal maps |
| uv0 | `float2` | 8 | textures |
| index | `u32` | 4 per index | everything |

- **Not interleaved, so each reader reads only its stream**: a BLAS build and a depth-only draw read 12 bytes a vertex, not 28.
- **A mesh takes one range `[base, base + n)` in every stream**, so one base vertex and one offset reach all of them.
- **Every mesh fills every stream**: a missing normal or tangent is generated, as sv generates tangent frames today, and a missing uv is zero.
  Optional streams would need an offset per stream or waste their space, for little.
- **Every other attribute stays in the bindless tables**: colors, uv1 and whatever else a mesh carries, pulled by the stages that read them.
- **Positions stay full floats in the mesh's own space**, so a large scene loses no precision and a BLAS builds from them unconverted.
- **Indices are always `u32`**: CAD meshes routinely pass 65,536 vertices, and one width keeps the central buffer one buffer.

### Culling on the GPU

**A cull kernel per frame writes the draws, so the CPU never walks the instances.**

- **Frustum culling** per instance against its world bounds.
- **Two-phase occlusion culling** against a depth pyramid:
  phase one draws what was visible last frame, builds the pyramid from that depth, and tests everything else against it;
  phase two draws what passed, which is what was disoccluded.
  Both passes draw depth alone, and the shading draws of both follow once the depth is complete.
- **Survivors are binned by material permutation**, a counting sort as the wavefront's shade sort is.
  Each permutation then gets one indirect draw with a GPU-written count, so the CPU records one draw per permutation, never per instance.
- **Cluster culling inside a mesh comes later**, with the LODs, since both want meshes split into clusters.

**Overdraw is answered by a depth prepass** (decision 4), built and measured in [raster.md](../raster.md#what-culling-and-the-prepass-measured).
The opaque draws lay down depth through the shared vertex stage, and the shading draws test against it at `greater_equal` without writing.
A cutout skips it and writes its own depth as it shades, since its depth depends on its material.

### LODs, later

A mesh carries several index ranges, coarser ones simplified from the finest.
The cull kernel picks one per instance by its projected error, so the draw is the same draw with another range.
The BLAS keeps the finest level: a trace sees true geometry, and raster and trace may then disagree by the error the LOD allows, which self-shadowing has to tolerate with a ray offset.

### What sg grows

- **Indirect draws with a count buffer** — [feat-sg-indirect-draws](../../../shaped-graphics/docs/issues/feat-sg-indirect-draws.md), following `dispatch_groups_indirect`.
- **The draw's identity inside an indirect draw.**
  A draw must learn which instance it is: the first-instance value, which dx12 exposes only from shader model 6.8, or a root constant through the command signature.
  Which one sg standardizes on is sg's design.
- **Refit** — [feat-sg-acceleration-structure-refit](../../../shaped-graphics/docs/issues/feat-sg-acceleration-structure-refit.md).
- **Compaction** — [feat-sg-acceleration-structure-compaction](../../../shaped-graphics/docs/issues/feat-sg-acceleration-structure-compaction.md).
  The TLAS keeps its per-frame rebuild; refit is for deforming meshes, compaction for memory.

## The thin G-buffer

**The raster pixel stage evaluates the full material once, then reduces it to a closure the lighting reads.**
The material body is the generated one the hit groups use, as raster's already is, so there is still no second BSDF.
What is new is `openpbr.reduce`: the full closure in, the reduced one out.

The reduced closure (decision 5):

- **diffuse albedo**, with subsurface and sheen folded in by their albedo;
- **specular color at normal incidence and one roughness**, isotropic;
- **a coat**: weight and roughness, with a fixed IOR;
- **shading normal and geometric normal**;
- **flags**: transmits, thin-walled, has emission.

What it loses: anisotropy, thin-film, the coat's own color and IOR, sheen's lobe shape, and subsurface beyond its albedo.
The converging wavefront keeps all of them.

Proposed layout, packed, about 28 bytes per pixel beside the hardware depth:

| target | format | holds |
|---|---|---|
| `emission` | rgba16_float | the surface's own emission; the lighting adds the rest into it |
| `normals` | rgba16_snorm | shading normal and geometric normal, octahedral |
| `albedo` | rgba8_unorm | diffuse albedo, coat weight |
| `specular` | rgba8_unorm | specular color, roughness |
| `extra` | rgba8_unorm | coat roughness, flags, two spare |
| `motion` | rg16_float | screen-space motion |
| `instance` | r32_uint | instance id, for the denoiser and for picking |

**The denoiser's guides become the lighting pass's output**, written in the formats `sr::reconstruct_inputs` wants, from the G-buffer it reads anyway.
So the G-buffer is the hybrid's own and can stay packed, and seven targets leave one under D3D12's eight.
Linear depth is reconstructed from the hardware depth.

## Lighting

**Inline tracing, in one pass**: one compute kernel at the upscaler's input extent where it is deferred, the material's own pixel stage where it is forward.

### Forward or deferred

**Both bind the tracer's own `tracer.traced` as group 0**, which holds the TLAS, the lights, the emitters, the instance table and the decals.
So both light through the tracer's light selection and its inline shadow ray, and only where the closure comes from differs.
`raster.drawn` cannot hold the TLAS, since a raster layer must draw on a device without ray tracing, and sg allows three groups.

- **Forward**: a hybrid vertex stage reads `traced` instead of `raster.drawn`, and the material's generated pixel stage lights its full closure, tracing as it goes.
  The depth prepass is what makes it affordable: each pixel stage runs once, for the visible surface.
  Its costs are a long pixel stage holding up the raster pipeline, divergent rays inside pixel waves, helper lanes on small triangles, and rays only ever at the raster's own resolution.
- **Deferred**: the pixel stage writes the thin G-buffer, and one compute kernel lights it.
  Its costs are the G-buffer's bandwidth and the reduced closure.

The images differ by what the reduced closure loses, and the frame times by everything above.

**Both are built**, as `sv::hybrid_routine` (rendering/hybrid_routine.hh) with `hybrid_lighting::forward` and `::deferred`, behind `render_mode::hybrid`.
**A smooth metal or mirror reads black and sparkling until phase 4**: it sees the environment only through the uniform environment sample, which seldom lands in its lobe.
`hybrid-test.cc` holds each, averaged over 128 frames, to the megakernel's direct light on the Cornell box with its blocks: the worst block is within 3.3% for both.

What they measured, in `raster-benchmark.cc` on Intel's Sponza at 1920x1080 on a laptop RTX 5070 in October 2026.
Both run occlusion culling and the prepass, include the TLAS build, and trace one environment sample with its shadow ray per pixel, since Sponza has no lights:

| frame | GPU time |
|---|---|
| raster, occlusion culling and prepass | 1.52 ms |
| hybrid, forward | 2.65 ms |
| hybrid, deferred | 2.35 ms |

**Deferred is about 11% faster here**, with four permutations of the plain `pbr` type, so the forward pixel stage carries little material code into its waves.
A scene of many lights, many permutations or heavy materials is what would move the comparison, and the reduced closure is what deferred pays in the image.
The raster rows of that run are slower than the prepass measurement in raster.md: the hybrid's frames, interleaved with them, run the GPU warmer.

### Why not the wavefront or a pipeline

- **The paths are short and fixed in length**: a primary hit, at most one indirect segment, a shadow ray at each.
  On shallow paths the wavefront's stored state costs more than it saves: the megakernel is about twice as fast on the Cornell box ([wavefront.md](../wavefront.md#what-it-measured)).
- **The wavefront's largest stage is the per-permutation shade**, ~40% of a frame, and decision 6 removes per-permutation code from secondary hits entirely.
  With no material code past the primary hit, there is nothing to sort, so one kernel serves every pixel.
- **A pipeline's hit groups exist to run material code at hits**, which this mode does not do.
  Inline traces also reach metal by the same SGL, as the wavefront's do.
- **The escape is the wavefront's own**: if a cutout-heavy scene measures inline traversal slower than a pipeline, a raygen with the same body is the fallback.

### Per pixel

- **Direct light**: one light chosen by today's power-weighted selection, one shadow ray, and its MIS weight against the indirect ray.
  The environment is sampled the same way.
- **The indirect ray**: one direction sampled from the reduced closure.
  Where it reaches a light or the environment, that counts with its MIS weight.
- **At its hit**: the simplified material, one next-event shadow ray, and the cache's radiance for everything beyond.
- **Smooth surfaces** follow a mirror chain for up to a few specular bounces before ending in the cache, since a coarse cache seen in a mirror reads as blur.
  How many is a measured setting.
- **Transmission** at the primary hit continues the ray through the surface, and the same chain applies; [raster.md](../raster.md#not-done-yet)'s depth peeling stays raster's own answer.

That is three rays per pixel in the common case, and fewer once the indirect half runs at a lower resolution ([Budget](#budget)).

### The secondary material

**Data, like the opacity record.**
An instance already carries where its opacity lives ([wavefront.md](../wavefront.md#cutout-is-data-not-code)).
An emitter also carries where its emission lives ([emissive-triangles.md](emissive-triangles.md)).
The secondary record adds three more of the same kind: base color, roughness, metalness, each a constant, a mesh attribute or a texture channel.
A material type declares them as it declares `opacity_attribute`, so the closest hit and every secondary hit read one fixed function.
A texture there is read at a coarse mip, since the cache blurs past it anyway.

## The radiance cache

### What the patents rule out

**Every hash-grid radiance cache is out**, which takes the best-known designs with it.
NVIDIA's [US 10,614,613 B2](https://patents.google.com/patent/US10614613B2/en), active until about 2038, claims as its claim 1:
quantize a path vertex's features, compute a hash of them, resolve collisions within a hash table, accumulate the path's contribution there, count, and average.
That is SHaRC, AMD's GI-1.0 world cache, Bevy Solari's world cache, and hashed path space filtering (Binder et al. 2019) alike.
An MIT-licensed implementation changes nothing, since the license is not NVIDIA's.
Its dependent claims add jitter before quantization, distance-dependent cell size, a checksum, moving averages and a level of detail; they bind only where claim 1 does.

Others to stay clear of, all NVIDIA's:

- [US 11,609,899 B2](https://patents.google.com/patent/US11609899B2/en): buffering concurrent updates, grouped per hash entry and applied by one thread each.
- [US 12,394,144 B2](https://patents.google.com/patent/US12394144B2/en): one representative point per hash cell, chosen by an atomic max, from which the cell's rays are traced.
- [US 2023/0325988 A1](https://patents.google.com/patent/US20230325988A1/en), pending: world-space irradiance by spatial hashing, combined with material-weighted screen-space filtering.
- [US 12,067,667 B2](https://patents.google.com/patent/US12067667B2/en): cached directional radiance steering the sampled direction, which is path guiding.
  **So the cache only ever ends a path; it never chooses a direction.**
- [US 11,756,254 B2](https://patents.google.com/patent/US11756254B2/en): light importance caching by spatial hashing, which light selection must not grow into.

**Counsel confirms the design below before phase 5 starts**, and the question to NVIDIA about ReSTIR is worth widening to US 10,614,613.
This section is an engineer's reading of the claims, not a legal opinion.

### The design: a directly addressed clipmap

**A camera-centered clipmap of dense 3D grids**, each level twice the cell size of the one inside it.
A cell's address is its integer coordinate, wrapped toroidally inside its level's grid, so nothing is hashed and nothing collides.
As the camera moves, a level scrolls: the slab of cells that wrapped around is cleared and refills.

- **A cell holds irradiance in six directions**, an ambient cube, so the normal is a lookup direction rather than a key.
- **A lookup** takes the finest level that holds the point, interpolates trilinearly between cells, and weights out a cell behind the surface by its center's side of the normal.
- **Starting size**: six levels of 64³ cells, six half-float RGB directions each, about 75 MB; measured, then tuned.

**It is fed by the frame's own rays, which is what makes it multi-bounce.**
The light gathered at each indirect hit — its next-event estimate plus the cache's own answer there — is added to that hit's cell.
So each frame's secondary light becomes the next frame's cached light, one bounce further each time.

- **The adds are integer atomics in fixed point**, since SGL has no float atomics, into a per-cell sum and count.
  A resolve pass then blends them into the stored irradiance and clears them.
- **A cell nothing reached fades by age**, so a light switched off does not linger.
- **Where the frame's rays leave cells empty**, a sparse set of extra paths from a strided subset of pixels runs a few bounces deeper to fill them; tuned by measurement.

**Leaking is its known weakness**: a cell straddling a thin wall averages both sides.
It shows less here than in a probe-only renderer, because the cache is read only past the first indirect hit, so contact and near-field light is always traced.

### Alternatives kept

- **A DDGI-style probe grid**: NVIDIA's application on it was abandoned, so the method is open, but its RTXGI implementation is not; it would be written from the paper.
  Diffuse alone, and it needs per-probe visibility to keep from leaking.
- **Irradiance caching after Ward (1988) and Křivánek (2005)**: sparse world-space records with error-bounded interpolation, long out of patent, and harder to make fast on a GPU.
- **A surface cache in Lumen's style**: no patent found, and by far the most engineering.

## Denoising and upscaling

**`sr::reconstruct_routine` picks the member, as for a traced layer.**

- **NVIDIA**: `dlss_rr`, which denoises and upscales in one; it needs the scaled frames to reach it, which [the roadmap](rendering-roadmap.md#upscaling-while-moving) lists as open.
- **metal**: MetalFX's temporal denoising upscaler, a new sr member; its exact API and OS floor are to verify against the SDK.
- **Elsewhere on dx12 and vulkan**: one of sr's own temporal denoisers, then FSR 3.1's upscaler.
  `fsr_rr` joins where it runs, RDNA 4 alone today.

The guides they read are the lighting pass's output ([The thin G-buffer](#the-thin-g-buffer)).

**When the camera stops, the image hands over to the converging wavefront** through the existing crossfade, as decided before this plan.

## Budget

**60 fps on a laptop RTX 5070 is 16.6 ms**, rendered at most at 1080p and upscaled to the output, up to 4K (decision 12).
On a 1920x1200 display at DLSS's quality ratio, that is about 1280x800 rendered, ~1M pixels.

An estimate, from the wavefront's measured costs on Sponza: ~3.5–4.5 ns per ray, extend and connect at full occupancy ([wavefront.md](../wavefront.md#what-it-measured)).

| layout | rays per frame | tracing |
|---|---|---|
| everything at the render extent: shadow, indirect, shadow at its hit | ~3M | ~12 ms |
| direct at the render extent, indirect at half of it per axis | ~1.5M | ~6 ms |

**So the resolutions mix by default**: the G-buffer and direct light at the render extent, the indirect ray and its shadow at half of it per axis.
The indirect light is upsampled to the render extent by depth and normal before the denoiser reads it.
With raster at a guessed 2–3 ms and reconstruction at a guessed 2–3 ms, the frame lands near 11–13 ms.
The per-ray figures carry the wavefront's stored-state traffic, which one inline kernel does not, so phase 3 is expected to beat them; phase 3 measures it.

## Phasing

Each phase lands with its tests, and its measurement on Sponza and on one large geometry scene.

1. **Central buffers and indexed draws.** The raster path and the BLAS builds move onto them; images unchanged.
   - 1a: positions and indices in two pools, every reader moved onto them, raster still pulling three corners per triangle.
   - 1b: indexed raster draws, the pixel stage taking `@primitive_id` and rebuilding its barycentrics from the triangle.
2. **sg indirect draws, then GPU culling** in raster mode: frustum first, then two-phase occlusion and the per-permutation bins.
3. **`render_mode::hybrid` with direct light and shadows alone**: the thin G-buffer, the lighting kernel, the guides, denoised.
   - 3a, done: both lightings at the routine level, tested and measured ([Forward or deferred](#forward-or-deferred)).
   - 3b, done: `render_mode::hybrid`, with `render_settings::hybrid_lighting` choosing forward or deferred, accumulating while still; the raster example shows both.
   - 3c: denoised, from the G-buffer slots it already writes.
4. **The indirect ray** with the secondary material, ending in the environment past its hit.
5. **The radiance cache**, ending the indirect ray in it, once counsel has confirmed its design.
6. **Reconstruction**: `dlss_rr` on the scaled stream, the MetalFX member.
7. **LODs and cluster culling.**

The reference for every image test is the converging wavefront, with a bound that names what this mode approximates: the reduced closure, the secondary material, and the cache's bias.

## Open questions

1. **How many specular bounces a mirror chain gets** before the cache, and whether glass gets more than mirrors.
2. **Whether the secondary material is good enough** (decision 6), judged by images once phase 4 draws.
3. **Whether half-resolution indirect holds up on fine geometry**, or wants a checkerboard at the full render extent instead; measured in phase 4.
