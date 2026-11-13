# The bound raster mode

**A plan, nothing built yet.**
It is sv's renderer for a device without binding arrays and without a ray-tracing pipeline: WebGPU first, phones included.
It has to look very good there and stay fast, so it approximates global illumination rather than stopping at direct light.
`render_mode::raster` stays what it is, the bindless raster mode, and this is a second raster mode beside it.

The decisions below were taken by the user in three discussion rounds on 2026-11-05.

## Why a second raster mode

**sv's materials reach every texture and buffer through bindless tables**, module `tracer`'s `binding bindless`, which requires `binding_arrays`.
WebGPU core has no binding arrays, so no sv mode draws there today ([raster.md](../raster.md#no-ray-tracing-needed)).
WebGPU's bindless proposals (`GPUResourceTable`, `sized_binding_array`) are experimental in Chrome as of November 2026.

**A non-bindless path stays needed after they ship**, since not every browser and device will have them.
So there are two raster modes: `raster` with bindless, and this one, which binds per draw.
Where a device has both, the bindless one is expected to be the faster, which a benchmark confirms or refutes.

## Decisions

1. **A new `render_mode`, named `raster_bound` for now.**
   The name is open to change; `portable` was the other candidate.
2. **A bind group per material.**
   A draw binds three groups: the frame, the material and the draw.
   Two binding profiles chosen by `binding_arrays` stay possible later, once this mode exists.
3. **One pipeline per material permutation, as today, with the generated WGSL shipped where the type is known.**
   Benchmarked against fewer pipelines before it is settled.
4. **The automatic mode chooses by capabilities, and an explicit override always exists.**
   A startup probe kernel and a frame-time governor are optional additions, not requirements.
5. **Antialiasing: MSAA on mobile, TAA on desktop**, both built and benchmarked so the choice has a measured cost.
6. **Forward shading, with tile-based GPUs in mind.**
   WebGPU has no subpasses and no framebuffer fetch, so a deferred G-buffer always leaves the tile; forward keeps the final color the only write.
7. **Mobile is a target**: Safari on iOS 26 and Chrome on Android ship WebGPU.
8. **Probe GI traced through the ray-query polyfill is the first indirect light**, and its speed is measured before anything is built on it.
9. **Every way of drawing a large assembly is measured** before one is chosen: direct calls, render bundles, GPU culling, merging per material, instancing.
10. **Samples are our own scenes**, built from typed-geometry or generated, so a benchmark scales them at will.
11. **Phones reach the samples over https on the LAN**, with a self-signed certificate trusted on each phone.
    WebGPU exists only in a secure context, so plain http from the dev machine gives a phone no WebGPU at all.
12. **Every checkpoint ends in something to look at**, handed to the user before work moves on.

## The frame

```
cull ─▶ main pass: forward shading, one bind group per material ─▶ resolve or TAA ─▶ composite
(GPU)   (vertex buffers, MSAA on mobile)                            (TAA on desktop)
         ▲
         └── reads: shadow maps, probe volume, environment, lookup tables (group 0)
```

**What the two raster modes share**: culling, the environment, and the lighting library — shadows, LTC, probes, screen-space passes as SGL modules and routines.
**What differs**: how a material reaches its resources, and how vertices arrive.

### Groups

| group | holds | changes |
|---|---|---|
| 0 frame | camera, lights, shadow maps, probe atlas, environment, lookup tables | once a frame |
| 1 material | the parameter block and its textures | per material |
| 2 draw | the instance's transform and ids, at a dynamic offset | per draw |

Group 3 is sg's reserved group on every backend.
WebGPU allows 16 sampled textures and 16 samplers per stage by default, so a material holds up to 8 textures, with group 0 holding about 6.
**The 18-sampler palette of `tracer.bindless` does not exist here**: a material's group holds the samplers its textures use.

### Vertices

**Vertex buffers rather than pulling from storage**, with positions in a stream of their own, as [hybrid-renderer.md](hybrid-renderer.md#one-vertex-buffer-one-index-buffer) proposes.
A tiler's binning pass reads only positions, and mobile drivers prefer fixed-function fetch.
sv's attributes may be per corner or per triangle, which a vertex buffer cannot say, so this mode expands them to per vertex at load, splitting vertices where needed.

### Drawing many parts

**A WebGPU draw is one call from wasm**, and multi-draw indirect is still behind a flag.
- **Render bundles** record the binds and indirect draws once and replay them every frame for almost no CPU.
- Indirect arguments are read when the bundle runs, so GPU culling still decides each frame, a culled draw being an instance count of 0.
- **Instancing** folds repeated parts and **merging** folds static parts of one material; merging costs per-part picking unless a part id travels with the vertices.
- Bindings change only between draws, recorded by the CPU: the GPU never chooses a pipeline or a bind group.

## Checkpoints

Each lands with its samples as web pages, and with what it measured.

| # | lands | sample |
|---|---|---|
| S0 | the polyfill throughput spike | rays per millisecond, per device |
| C0 | the async frame API, the web harness, `raster_bound` untextured | OpenPBR spheres, lit directly |
| C1 | material bind groups, textures, glTF over fetch, instancing, the assembly benchmark | a textured glTF, a CAD assembly |
| C2 | probe GI, sized by S0 | an assembly lit indirectly |
| C3 | shadow maps, LTC rect lights, a prefiltered environment | a showroom |
| C4 | screen-space AO, specular occlusion and reflections | a showroom with a glossy floor |
| C5 | a mobile pass: tiles, MSAA against TAA, compile cost | everything, on a phone |

### S0: how fast the polyfill traces

The polyfill builds its tree over the primitive order with no spatial sort ([raytracing-polyfill.md](../../../shaped-graphics-language/docs/raytracing-polyfill.md#building)).
Nothing has measured its speed.
S0 measures it before probe GI is designed around a guess.

- **Scenes**: a coherent mesh, a triangle soup, and a generated assembly of instances.
- **Rays**: camera-coherent, probe-like over the sphere, and short occlusion rays.
- **Trees**: as built today, and sorted by Morton code, to size that fix.
- **Devices**: a laptop RTX 5070, an RTX 5080, a Mac laptop, and phones.
- **Out**: rays per millisecond per scene, ray kind and device, and the pool's size per scene.

It needs no sv: an sg and SGL web page beside `sgl-cube`, and a nexus benchmark of the same kernels under node with Dawn.

### C0: the generated material body, reading bound buffers

**Decided 2026-11-06**: the pools hold every backend's blocks and attributes, and C0 draws no decals.
The material body stays the generator's, as raster.md requires; what changes is the spelling of its loads.
Today every load indexes `tracer.bindless`: the vertex and index pools, the instance's parameter block, each attribute's buffer, each texture.

- **Everything a frame reads by slot becomes a pool bound in group 0**: the vertex and index pools sv already has, and two new ones.
  A **parameter pool** holds every instance's block, so `param_offset` is absolute and `param_buffer` unused.
  An **attribute pool** holds every attribute, so a descriptor's offset is absolute and its buffer unused.
- **Group 0 is `raster.drawn`'s members plus those four pools**: frame, instances, lights, background, four pools.
  The environment sits in the group's constant buffer, so that is seven storage buffers, and sg's shader diagnostics take the eighth, WebGPU's default for a stage.
  That is why decals wait: they would make ten.
- **Group 1 is the material's**, its textures and their samplers, which C1 fills; C0 materials are untextured.
- **Group 2 stays `slug.tables`**, for decals once group 0 has room for their records, by packing them into one buffer.
- **The generator gains a third spelling, `bound`**, beside the traced and raster ones: `bound.params.load(...)` where it wrote `tracer.bindless.buffers[block].load(...)`.
- **A module `raster_bound`** holds the group and the pixel stage's surroundings, calling `raster`'s lighting where it reads only `drawn`.
- **The vertex stage pulls positions from the pool**, as `raster` does; vertex buffers are C1's, with the expansion of per-corner attributes.
- **No GPU culling in C0**: one indexed draw per item, recorded by the CPU.

**The managers pool on every backend**, so there is one resource model and one layout behind all three spellings.
The traced and raster spellings then read the same pools through a bindless slot, and `bound` through a bound buffer.

**Status 2026-11-06**: `render_mode::raster_bound` draws, through `sv::raster_bound_routine`, and `automatic` picks it where a device lacks `binding_arrays` and ray tracing.
raster-bound-test draws the Cornell box through it on a dx12 context withholding both, indexed and pulled.
Not drawn yet: frame meshes over their own index buffers ([feat-sv-raster-bound-missing-geometry](../issues/feat-sv-raster-bound-missing-geometry.md)).
Quadric batches draw since 2026-11-11, their records pooled in the vertex pool; decals too, as lanes of group 0's constant buffer, up to 1024.

**Textures, the first part of C1 (2026-11-09)**: a textured material's bound stage declares a group of its own, `sv_textures`, one texture and one sampler per texture slot, as group 1.
- Its layout is read off the compiled stage's reflection, so nothing on the host spells the generated group again.
- The samplers are bound rather than static, and they are the permutation's, `material_permutation::bound_samplers`, since a sampler is part of what a permutation is generated from.
- Each draw creates the group over its instance's textures, `gpu_resource_manager::instance_textures`, with the parameter block's placeholders while a texture streams.
  A group per draw every frame is the simple shape; caching it, or a render bundle, is the assembly work's.

**The web sample runs** (`examples/graphics/web-viewer`): the OpenPBR sphere grid on the page's canvas, in Firefox and Edge.
- A device without `primitive_index` draws pulled: no index buffer, three corners a triangle.
  That is every WebGPU device today, since the webgpu backend does not request the feature.
  Its vertex stage reads each corner's index out of the pool, and hands the pixel stage its triangle as a flat varying.
- `sr::mix_routine` needs `readwrite_image_formats`, so the viewer prewarms it only where the device reports the feature.
- A page only hints at a GPU, and sg asks for the high-performance one.
  Firefox ignores that today and takes a laptop's integrated GPU ([Bugzilla 2016063](https://bugzilla.mozilla.org/show_bug.cgi?id=2016063)); Windows' per-app graphics setting overrides it.
  On the development laptop that is 40 frames a second on the AMD iGPU against 336 on the RTX 5070 ([perf-sv-raster-bound-shading-cost](../issues/perf-sv-raster-bound-shading-cost.md)).
  The bound mode does not use it.

**glTF over fetch, the next part of C1 (2026-11-09)**: the page's `?model=<glb>` is fetched through `cnet::fetch_http_client` and imported by `sv::asset_loader`.
- The Research Scenes' modern hall draws in Firefox with every material, at the speed of the unlit sphere grid.
- Its light is 13 emissive meshes and no lights, which the bound mode cannot light with.
  The example turns each into a rect light ([feat-sv-raster-bound-emissive-instances](../issues/feat-sv-raster-bound-emissive-instances.md)).
- The opaque draws lay down their depth first in a pass of their own, through the shading draws' own vertex stage; a cutout writes its depth as it shades.
  On the hall that saved 8% natively, since its view has little overdraw.
- **A rect light is two closure evaluations a pixel, not sixteen** (`raster.rect_light`).
  Its diffuse half is in closed form over the four edges, its specular half at the most representative point, widened as Karis widens a sphere light.
  The 4x4 grid stays as `rect_lights_by_grid`, the reference.
  On the hall's 13 lights that took a native frame from 6.15 to 0.79 ms, and Firefox from 40 frames a second to 327.
  This is ahead of C3's LTC rect lights, which stay the answer if the representative point's highlights fall short.

**Shadows and reflections, ahead of C3 and C4 (2026-11-10)**: `sv::shadow_settings` picks the technique, and `sv::screen_space_reflections` adds mirror reflections.
- **Shadow maps are the default** (`raster_bound_shadow_maps.sgl`): a 1024 tile per rect light in one depth atlas, a 140 degree frustum along its normal, read through percentage-closer soft shadows.
  The atlas is redrawn only when the instances or the lights hash differently, so a still scene draws it once.
  On the hall's 13 lights it costs 0.35 ms a frame in Firefox, against 0.7 ms for the traced share, and keeps no history, so nothing trails.
  PCSS is Fernando's 2005 sketch; no patent was found.
- **Traced shadows stay as a choice**, one ray a half-size pixel accumulated over frames, since they cover every light where the maps cover the first 16 rects.
  A traced scene's TLAS is kept while its instances are unchanged: rebuilding it every frame made 36 bind groups a frame that Firefox frees only at a collection, and the collections stalled it.
- **Screen-space reflections** (`raster_bound_reflections.sgl`) march each half-size pixel's mirror ray across the prepass's depth, as McGuire and Mara's 2014 paper does.
  Where a ray lands, they read last frame's image.
  Last frame's image is the layer's color target before the shading overwrites it, so nothing is copied.
  The shading trusts them in place of the environment's reflection on surfaces up to roughness 0.15, fading out by 0.45.
  The normal is the depth's, so a normal map does not bend a reflection yet.
- **Every half-size signal maps texel `h` to full-size pixel `2h`, centered at `2h + 0.5`.**
  A quarter-texel slip there made a still history slide toward the bottom-right without end ([bug-sv-raster-bound-shadow-smoke-on-motion](../issues/bug-sv-raster-bound-shadow-smoke-on-motion.md)).
- The web-viewer loads the Research Scenes' Country Kitchen (CC-BY 3.0, credited on the page through `?credit=` and `?license=`) as the scene to judge reflections on.

**A dome's lobes are lights (2026-11-12)**: each lobe of an analytic dome becomes a distant disc for the frame, and the environment's SH is projected from the dome without them.
- The SH blurred a studio's softboxes into a glow nothing shadowed, so a studio-lit model lost every crease the traced modes show; the Research Scenes' dragon read as smooth.
- A disc at the lobe's peak radiance over a cap of the lobe's own integral, `2 pi (1 - e^(-2k)) / k`, delivers the lobe's irradiance to a surface facing it.
- The traced shadows and the indirect's shadow rays aim within the cap, so a disc's shadow softens with its size, a sun's included.
- A mirror sees a lobe as the highlight of one direction rather than the softbox's shape, until the reflection term reads the lobes too.
- Turning an authored lobe into a light is no extraction from an image, and a search for a patent on it found none.
  US 9,928,641's claims, shadow-map channels over a BSP's light lists, read on nothing here.

### Probe GI

A probe volume in the style of DDGI (Majercik et al. 2019), written from the paper: irradiance and visibility per probe, a fixed ray budget a frame spread over several frames.
NVIDIA abandoned its application on the method ([hybrid-renderer.md](hybrid-renderer.md#alternatives-closed)); the RTXGI code is not used.
- **A probe's ray hit** is lit by the shadow maps and the previous frame's probes, which makes it multi-bounce, and reads the secondary material: base color, roughness, metalness.
- **On dx12 and vulkan** the same probes trace on hardware, so the image is one design across backends.
- **On a phone** the probes may update much more slowly, or be baked; decided once S0 has numbers.
- **Every other technique is checked for patents before code**, as the hybrid's radiance cache is.

### Compile cost

WebGPU always compiles WGSL from source when a pipeline is created, phones included, so materials may compile at run time.
What costs is the time: a phone creates a pipeline far more slowly than a desktop, and SGL's own compile runs inside wasm.
- Shipping the generated WGSL of known material types removes the SGL step.
- The stand-in permutation sv already draws while a material compiles hides the rest.
- An OpenPBR uber-shader is the measured alternative if pipelines stay too slow.

## What the libraries grow

- **sg: render bundles**, with a native counterpart per backend — [feat-sg-render-bundles](../../../shaped-graphics/docs/issues/feat-sg-render-bundles.md).
- **sg: WebGPU's `indirect-first-instance`**, so an indirect draw starts at an instance row.
  [feat-sg-webgpu-indirect-first-instance](../../../shaped-graphics/docs/issues/feat-sg-webgpu-indirect-first-instance.md) files it.
- **sg: indirect indexed draws work on webgpu** — [bug-sg-webgpu-indirect-indexed-draw-under-node](../../../shaped-graphics/docs/issues/bug-sg-webgpu-indirect-indexed-draw-under-node.md).
- **sg: a sorted polyfill tree, before probe GI.** S0 measured an unordered mesh tracing up to 2500 times slower than the same one sorted.
  Built: [perf-sg-polyfill-unsorted-tree](../../../shaped-graphics/docs/issues/perf-sg-polyfill-unsorted-tree.md) has the design and the numbers.
  With it, sorted probe rays run at 30 to 38 Mrays/s on a laptop RTX 5070 in any triangle order.
- **sv: a frame API that never blocks** — [feat-sv-async-frame-api](../issues/feat-sv-async-frame-api.md).

## Baked scenes

**A baked scene is an issue for now, not a checkpoint** — [feat-sv-baked-scenes](../issues/feat-sv-baked-scenes.md).
The Shelley pipeline already bakes path-traced lightmaps, unwrapping, denoising and compressing them, and is the first candidate producer.
Where baked data lives is open; a new file format is a decision, not a migration.

## Open questions

1. **The largest assembly that must stay interactive.** Unknown, so the assembly benchmark sweeps 1k to 100k parts.
2. **Whether `raster_bound` ever beats `raster` on a device that has both.**
3. **Probe GI on phones**: live and slow, or baked; after S0.
