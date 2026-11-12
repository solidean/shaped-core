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
**Phase 4's bounce is in**: one direction drawn from the closure, and the environment along an escape weighed against the environment sample.
A hit is lit as a surface of its base color, roughness and metalness.
The secondary material is data, `sv::material_secondary`: the base color, roughness and metalness each type names.
They are read through the instance record by `tracer.instance_base_color` and its neighbors, as the emitter reads its color.
The bounce reaches the lights too, weighed by MIS against the light sample as the tracer weighs them: the rects it crosses, the discs it escapes into, and an emissive mesh it meets.
`hybrid-test.cc` holds both to the megakernel's two bounces on the Cornell box, within 5.2% for the worst block.
`hybrid-test.cc` holds each, averaged over 128 frames, to the megakernel's direct light on the Cornell box with its blocks: the worst block is within 3.3% for both.

What they measured, in `raster-benchmark.cc` on Intel's Sponza at 1920x1080 on a laptop RTX 5070 in October 2026.
Both run occlusion culling and the prepass, include the TLAS build, and trace one environment sample with its shadow ray per pixel, since Sponza has no lights:

| frame | GPU time |
|---|---|
| raster, occlusion culling and prepass | 1.52 ms |
| hybrid, forward | 2.65 ms |
| hybrid, deferred | 2.35 ms |

**With the bounce of phase 4, deferred is about 40% faster**: 3.87 ms against forward's 6.34 ms in the same run, the denoiser not counted.
A bounce in a random direction diverges far worse inside a pixel stage, which holds up the raster pipeline, than in a compute kernel.
Both stay as options for now; `examples/hybrid-sponza.cc` shows them beside the wavefront.

Before the bounce, **deferred was about 11% faster**, with four permutations of the plain `pbr` type, so the forward pixel stage carries little material code into its waves.
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
No cache code is written before that answer.
This section is an engineer's reading of the claims, not a legal opinion.

### The design: a directly addressed clipmap

Settled in a design review on 2026-10-29, as the design counsel is asked about.

**A camera-centered clipmap of dense 3D grids**, each level twice the cell size of the one inside it.
A cell's address is its integer coordinate, wrapped toroidally inside its level's grid, so nothing is hashed and nothing collides.
As the camera moves, a level scrolls: the slab of cells that wrapped around is cleared and refills.

**The cache answers indirect light only.**
The secondary hit keeps its own light and environment samples with their shadow rays, so lamp light and shadows seen through a bounce stay sharp.
The cache adds `base_color / π` times the irradiance it holds there, and holds only light that bounced at least once more before arriving.
So the light that changes fastest and leaks worst, a lamp's direct light, is never cached.
Caching everything would save two of the pixel's five rays; it stays a later option for distant hits, once the cache's bias is measured.

**A cell is an ambient cube**: six RGB irradiances, for a surface facing each axis direction, at half precision.
The two sides of an axis-aligned thin wall face opposite ways, so they land in opposite faces and do not mix.
The representation stays behind one SGL function pair, `deposit` and `irradiance`, so first-order spherical harmonics can be measured against it.

**It is fed by the frame's own rays, which is what makes it multi-bounce.**
- **The pixel's own bounce feeds the cell at the pixel's surface**, at no extra ray.
  What the secondary hit sends back — its direct light plus the cache's answer there — is light arriving at the pixel's surface from another surface.
  It is added weighted by the cosine over the pdf of the direction drawn.
  The sky and lamps the bounce itself reached are direct light, and are left out.
- **One pixel in N traces one more bounce from its secondary hit**, with its direct light, to feed the cells off screen; N starts at 16, about 190k rays and 0.25 ms on Sponza.
- **Cells do not trace rays of their own.**
  That would spend rays on cells nothing reads, and choosing one point per cell by an atomic max is US 12,394,144 B2.

So each frame's bounced light becomes the next frame's cached light, one bounce further each time.

**The adds are 32-bit integer atomics in fixed point**, since SGL has no float atomics, into a per-cell sum and count.
- **Every sample is clamped, and a cell takes at most K samples a frame**: the count's atomic add returns the count before, and a sample arriving past K adds nothing.
  A wall can put a hundred thousand samples into one coarse cell, so without the cap a bright scene would wrap the sum silently.
  With it, overflow is impossible by construction, and the fixed-point scale follows from K and the clamp.
- **No sort and no grouped updates**: applying concurrent updates grouped per cache entry by one thread each is close to US 11,609,899 B2.

**A resolve pass blends each cell's frame into its history** and clears the sums: `stored = mix(stored, frame_mean, max(1 / frames_fed, 1 / N))`, N starting at 32.
That is about half a second of lag at 60 fps.
A cell nothing reached ages and fades to zero, so a light switched off does not linger.
Forgetting faster where a frame disagrees with the history by more than its noise is the additive refinement, should switching lights show a lag worth a second moment per value.

**A lookup is one jittered read.**
The point moves by a random offset of up to half a cell along the surface, and slightly off it along the normal, and the one cell it lands in is read: three faces of one cube.
The jitter averages over frames and neighbors, and the denoiser removes the rest; a trilinear blend would be 24 scattered reads at the frame's least coherent point.
**The level matches the bounce's footprint**: the one whose cell is about the width of the ray cone where the bounce landed, so a long or rough bounce reads a coarse, well-fed cell.
Any level can be read at any point, so a feed sample adds to every level containing it, and the cap turns most coarse adds away early.

**A metal or glossy secondary hit multiplies the cached irradiance by its whole reflectance**, diffuse and specular together.
The energy stays roughly right; a polished secondary shows a blur where a reflection would be, seen through one bounce.
A mirror chain past a smooth secondary is the answer if an image shows that matters.

**Starting size: six levels of 64³.**
The stored cubes are 36 bytes a cell, 54 MiB; the frame's sums, six faces of RGB as `uint` plus the count, 76 bytes a cell, 114 MiB; 168 MiB together.
With a 10 cm finest cell it reaches ±3.2 m at that detail and ±102 m at 3.2 m.
Sponza's transient upload peak is what to lower if memory is short, not the cache; sums kept only for the cells a frame feeds are the lever after that.

**Leaking is its known weakness**: a cell straddling a thin wall averages both sides, and the cube separates only axis-aligned ones.
It shows less here than in a probe-only renderer, because the cache is read only past the first indirect hit, so contact and near-field light is always traced.

**Two tests prove it.**
- **A closed glowing sphere**: the camera inside a closed matte sphere of albedo ρ emitting `Le` everywhere sees `Le / (1 − ρ)` in every direction, `5 Le` at ρ = 0.8.
  Without the cache the hybrid stops at `Le (1 + ρ + ρ²)`, 2.44 `Le`, so the test pins the bounce sum exactly, and the frames it takes measure the lag.
- **Sponza against the converged wavefront**, within a bound, for leaks and blur.

Two more pin its edge cases: a cell fed far more than K bright samples in a frame reads back within bound, and switching the only light off reaches near zero within the frames the resolve promises.

### Alternatives closed

- **A DDGI-style probe grid**: NVIDIA's application on it was abandoned, so the method is open, but its RTXGI implementation is not; it would be written from the paper.
  Diffuse alone, its probes trace their own rays every frame whatever the camera sees, and a probe inside a wall has to be moved or switched off.
  It stays the fallback if leaks measure badly, since it keeps the clipmap's feed and lookup interface.
- **Irradiance caching after Ward (1988) and Křivánek (2005)**: sparse records placed where an error bound asks for one, long out of patent.
  Placing a record needs a search over the existing ones, and on a GPU that search is usually a hash grid, which is the patented thing again.
- **A surface cache in Lumen's style**: light stored in texture cards covering each mesh, so nothing leaks, and memory follows surface area rather than empty space.
  It needs a card-placement pass per mesh when the mesh is first placed, which is hard on CAD geometry with thin parts and thousands of small meshes.
  It also needs a card atlas, and a lookup through the hit instance's cards.
  By far the most engineering, for a leak the ambient cube and the first traced bounce already ease.

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

### What a whole frame measured

`hybrid-benchmark.cc` times the viewer's whole moving frame on Sponza, output 1920x1200 at the quality preset, the camera walking the nave.
It breaks the frame down by stage through `sv::impl::gpu_mark`, a stand-in for GPU scopes in sg ([debt-sv-gpu-stage-marks](../issues/debt-sv-gpu-stage-marks.md)).
Each contender runs alone: four views' histories and path state at once overflowed the laptop's memory, and every number came out near double.

**A laptop GPU's heat moves these numbers by a factor of two**: the same frame measured 15.7 ms after three heavy contenders and 8.5 ms on its own.
So a comparison is only good between runs of one session, each measured cool.

Hybrid deferred with svgf and FSR, cool, on the laptop RTX 5070 in October 2026:

| stage | GPU time |
|---|---|
| TLAS build | 0.18 ms |
| raster: culling, prepass and the thin G-buffer | 0.54 ms |
| deferred lighting | 5.16 ms |
| blend into the mean | 0.05 ms |
| svgf and FSR | 2.47 ms |
| **the whole frame** | **8.5 ms** |

What the lighting's parts cost, each measured by removing it alone:
- the secondary hit's light and environment samples, about 3.0 ms;
- the cutout test in every ray, about 2.5 ms, on Sponza's one BLEND material alone;
- the primary light sample, about 2.2 ms;
- the secondary base color's texture read, 0.3 ms.

**The cutout cost is the candidate decisions, not their texture reads**: forcing every instance opaque recovers all of it, and sampling the opacity at mip 4 none.
**That material is a dirt decal whose opacity is a soft mask**: 79% of its texels lie strictly between 0 and 1, and it keeps coverage rather than thresholding.
An exact opacity micromap would leave those micro-triangles unknown, so it would recover little of the cost.
**The hybrid's rays skip an instance that keeps coverage and lights nothing**, through the TLAS instance mask, while every other trace still meets it.
Such a material is a decal over a surface, whose shadow and bounce barely move; a thresholded cutout, foliage above all, stays traced.
On Sponza that halved the deferred lighting pass, 6.7 to 3.4 ms, and took a dlss_rr frame alone on the GPU from 11.5 to 8.1 ms.
Opacity micromaps remain the answer for thresholded cutouts, whose texels are mostly exactly 0 or 1 — [feat-sg-opacity-micromaps](../../../shaped-graphics/docs/issues/feat-sg-opacity-micromaps.md).

**Two numbers per contender, since back to back and alone disagree**: throughput over the wall clock, and a breakdown with each frame alone on the GPU.
Back to back, it holds at most 2 frames in flight, as a windowed viewer's swapchain does.

**The CPU limited the frame, and most of it was the driver creating textures.**
Sampled, 14 of the 21 ms a frame took to record went to creating about six transient textures and freeing the last epoch's, each blocking in the driver for 1 to 3 ms.
sg now places transient textures in a per-epoch heap, as it does transient buffers.
Before and after, in one session:

| contender | frame before | frame after | recording before | recording after |
|---|---|---|---|---|
| hybrid deferred, svgf and FSR | 21.0 ms | **9.7 ms** | 14.7 ms | 5.1 ms |
| hybrid deferred, dlss_rr | 24.1 ms | **13.6 ms** | 15.6 ms | 5.6 ms |
| hybrid forward, dlss_rr | 20.1 ms | 17.8 ms | 11.8 ms | 6.1 ms |

About 1.5 ms of the remaining 5 ms of recording was the instance records: `describe_instance` rebuilt every item's parameter block, twice a frame.
A block is now built once per epoch per instance record, which items with the same material share, and recording fell to 4.1 to 4.6 ms.
The scene is walked once a frame too: `acquire_trace` walks it to acquire before the snapshot, and the trace takes that walk rather than repeating it.

**dlss_rr itself costs about 4.2 ms**, measured alone after a short run, where the whole frame is 10.8 ms.
After a long run its frames slow to about 35 ms, the hybrid's own stages too, which nothing in them explains.
**It is GPU memory spilled into system memory**, which the dedicated usage alone does not show.
The process holds 6.5 GB while Sponza uploads, its every texture uncompressed at once, and Windows demotes allocations at that peak.
With dlss_rr's resources on top, 2.2 GB stayed in shared system memory against 0.38 GB with svgf, so every pass read over PCIe.
Throttling was the same in both runs, and the clocks within 3%.
[perf-sv-texture-upload-peak](../issues/perf-sv-texture-upload-peak.md) is the fix.

Measured warm, forward's raster pass, which is its lighting, took 48.5 ms against deferred's 19.5 ms of lighting in the same run, and dlss_rr 10.4 ms against svgf and FSR's 2.3 ms.
Both want a cool measurement before they decide anything.

### The White Room at 1920x1080

The same benchmark draws the White Room as research-studio does by default: hybrid deferred at the native scale, the automatic denoiser (dlss_rr) on every frame, the room's own lights and camera.
**The frame is the GPU's, about 21 ms of it**: the CPU records in 3.5 ms and waits out the rest.
Each frame alone, on the laptop RTX 5070 in November 2026:

| stage | still | orbiting | orbiting, quality scale |
|---|---|---|---|
| TLAS build | 0.11 ms | 0.10 ms | 0.10 ms |
| raster: culling, pyramid, depth and shading | 0.75 ms | 0.70 ms | 0.45 ms |
| deferred lighting | 13.3 ms | 12.6 ms | 5.6 ms |
| guide blend | 0.33 ms | — | — |
| blend into the mean | 0.42 ms | 0.47 ms | 0.08 ms |
| dlss_rr | 6.9 ms | 6.9 ms | 3.6 ms, upscaling from 1280x720 |
| **the whole frame** | **21.9 ms** | **20.8 ms** | **9.9 ms** |

What the lighting's parts cost, measured by cutting the kernel short after each:
- the primary hit's light sample and its shadow ray, about 2.5 ms;
- the bounce ray, its glass passage included, about 2.4 ms;
- the bounced surface's material, light sample and environment sample, about 4.5 ms;
- the next and last rays that weigh the lights against the BSDF, about 3.1 ms.

Rays dominate it: with every trace missing and every shadow ray arriving, the kernel took 3.4 ms.
Cutouts cost nothing in this room, 12.5 ms with every instance opaque, and reading the bounced surface's material through its parameters cost nothing either.
Three savings followed, each leaving the image as it was:
- an environment sample toward a direction the environment sends nothing from returns no sample, so it traces no shadow ray;
- a third surface nothing follows is no longer prepared, since everything lit from it came to zero;
- an environment that is black everywhere skips both environment samplers whole, the bounced surface's BSDF sample with them.

The deferred lighting went from 12.7 to 8.3 ms, and the still frame from 21.9 to 16.8 ms.

## Phasing

Each phase lands with its tests, and its measurement on Sponza and on one large geometry scene.

1. **Central buffers and indexed draws.** The raster path and the BLAS builds move onto them; images unchanged.
   - 1a: positions and indices in two pools, every reader moved onto them, raster still pulling three corners per triangle.
   - 1b: indexed raster draws, the pixel stage taking `@primitive_id` and rebuilding its barycentrics from the triangle.
2. **sg indirect draws, then GPU culling** in raster mode: frustum first, then two-phase occlusion and the per-permutation bins.
3. **`render_mode::hybrid` with direct light and shadows alone**: the thin G-buffer, the lighting kernel, the guides, denoised.
   - 3a, done: both lightings at the routine level, tested and measured ([Forward or deferred](#forward-or-deferred)).
   - 3b, done: `render_mode::hybrid`, with `render_settings::hybrid_lighting` choosing forward or deferred, accumulating while still; the raster example shows both.
   - 3c, done: denoised through the traced layer's own flow, its G-buffer slots serving as the guides.
     A temporal member reads each frame alone and a spatial one the mean, with the crossfade between; no split signal and no upscaling yet.
4. **The indirect ray** with the secondary material, ending in the environment past its hit.
   Done, lights reached by the bounce and the secondary's roughness and metalness included.
   A refracted bounce crosses a solid, bending out at the first surface it meets inside, and continues from there; a thin wall's goes straight on.
   Every solid glass instance it meets after that is refracted through too, at index 1.5, up to six; the instance record's `glass` word says which are.
   What an interface reflects is lost, as is a crossing that ends in total internal reflection: a glass sphere under a uniform sky reads 0.96 of it at its center, 0.88 over its disc.
   Glass spends no bounce: the surface a bounce meets past it is lit as a primary hit is, by direct light and one plain bounce of its own.
   That is what keeps a hollow vessel in an indirectly lit room from going black, Veach Ajar's teapot, and it adds four trace sites to each lighting.
   The deferred kernel's glass is clear: the thin G-buffer keeps no medium and no thin wall.
5. **The radiance cache**, ending the indirect ray in it, once counsel has confirmed its design.
   The design is settled and waits on counsel; the glowing-sphere test comes first, since it tells a bug in the feed from a weakness of the design.
6. **Reconstruction**: `dlss_rr` on the scaled stream, the MetalFX member.
   Begun: a moving hybrid frame is drawn at the upscaler's input extent, jittered as a traced one is, and upscaled through the traced layer's own reconstruct path.
   A still one is jittered too, one Halton (2, 3) offset per sample of its mean, so the mean converges to each pixel's coverage rather than to an aliased image.
   `raster.frame_constants::jitter` moves the image in clip space so a pixel's center lands where its jittered ray would.
   `dlss_rr` takes that stream as it is, upscaling by itself at its own ratio and jitter, and a test runs it beside svgf with FSR.
   Left: the MetalFX member.
7. **LODs and cluster culling.**

The reference for every image test is the converging wavefront, with a bound that names what this mode approximates: the reduced closure, the secondary material, and the cache's bias.

## Open questions

1. **How many specular bounces a mirror chain gets** before the cache, and whether glass gets more than mirrors.
2. **Whether the secondary material is good enough** (decision 6), judged by images once phase 4 draws.
3. **Whether half-resolution indirect holds up on fine geometry**, or wants a checkerboard at the full render extent instead; measured in phase 4.
