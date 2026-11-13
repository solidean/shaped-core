# The raster path

**Built, and a choice rather than the default.**
A layer whose `render_settings::mode` is `render_mode::raster` is drawn by sg raster pipelines instead of traced: the same scene, the same OpenPBR materials, lit directly.
It is raster alone for now, and it is shaped as the base the interactive hybrid mode grows from.
That mode is rasterized primary visibility, then ray-traced lighting through a radiance cache ([rendering-roadmap.md](plans/rendering-roadmap.md)).
This document is what a raster frame draws, how it shares its materials with the tracer, the G-buffer seam a hybrid lighting pass reads, and what is not done yet.

## What a frame is

```
cull ─▶ depth of every opaque draw ─▶ one indexed draw per triangle item ─▶ environment where nothing was drawn
        (vertex stage alone)           (vertex: pull the vertex, project)     (depth test against the far plane)
                                       (pixel: @primitive_id, barycentrics, the material, then raster.shade)
```

- **The GPU culls first**: `raster_cull.sgl` tests each item's box, placed by its instance, against the camera's view.
  It writes the item's draw arguments, with an instance count of 0 for one out of view, and the CPU records one `draw_indexed_indirect` per material permutation.
  A draw's first instance is its row of the instance table, which the vertex stage takes as `@first_instance`.
- **A layer culls by occlusion too, in two passes.** The first draws what its record says was visible last frame and is in view.
  A pyramid of the farthest primary depth under each texel is built from what it left, and the second pass tests every item against it.
  That pass draws what is visible and was not drawn first, then the environment, and records what is visible for the next frame.
  A wrong record costs a draw and never a hole, since whatever the first pass misses the second catches; the record restarts when the count of draws changes.
- **The opaque draws lay down their depth first**, through a depth-only pipeline over the same vertex stage, as one indirect draw.
  The shading draws then test against it at `greater_equal` without writing it, so each pixel is shaded once, by the surface that is visible there.
  The same vertex stage computes the same depth in both pipelines, which is what lets the equal test through.
  A cutout's depth depends on its material, so its bins draw after the opaque ones, skip the prepass and write their depth as they shade.
  With occlusion culling, both passes lay down depth, the pyramid is built from it, and the shading draws of both passes follow in one scope.
  `raster_frame_desc::depth_prepass` turns it off, which is for measuring what it saves.
- **One indexed draw per scene item**, through the mesh manager's central index pool, at the mesh's first index and base vertex; no vertex buffer is bound.
  The vertex stage, `surface_vs` of [raster_pass.sgl](../shaders/raster_pass.sgl), pulls its vertex out of the central vertex pool.
  So one vertex stage serves every mesh, whatever attributes it carries.
  It hands on the point in the mesh's own space and in the world, since a shared vertex cannot say which triangle it is.
- **The pixel stage is generated per material permutation**, `raster_source` beside the hit group and the wavefront's shade kernel.
  It takes its triangle from `@primitive_id`, which requires the device's `primitive_index`, and finds its barycentrics from the object-space point.
  Then it builds the hit's `material.shading_context`, evaluates the material, and hands the surface to `raster.shade`.
- **The depth test is reversed.** A surface tests at `near / depth`, cleared to 0 at infinity; the prepass passes `greater`, a shading draw `greater_equal`.
  A float's spacing shrinks toward 0 as fast as that does, so the step it resolves stays about 6e-8 · depth wherever the surface is.
  The standard `1 - near / depth` would step by about 6e-8 · depth² / near instead: 6 units at 1000 with the default near plane of 0.01.
  It would also round to exactly 1 past about 3e7 · near, 3e5 units at the default, where the environment would draw over geometry.
  Only the pass's own depth is reversed: `primary_depth` stays `1 - near / depth`, so nothing that reads it changes.
- **The environment** is a fullscreen pass on the far plane, at depth 0 and tested `greater_equal`, so it lands only where no surface was drawn.
- **Every frame is final.**
  The layer's accumulation slot holds `accumulation_sample_cap` samples the frame it lands, so `view_ref::is_accumulation_converged` holds at once and a capture finishes on its first drawn frame.
  A frame drawn through a stand-in for a material still compiling is not final: it restarts next frame, as a trace with such a stand-in does.
  A stand-in for a material that failed for good is final, since it is the image the layer will keep drawing.
- **Nothing denoises.** `render_settings::reconstruct` is ignored, and neither the denoised slot nor the frame-samples slots are declared.

**The shading draws are one of three**, by `raster_frame_desc::shading`: `lit` is this path.
`thin` and `traced` are the hybrid's deferred G-buffer and forward lighting ([hybrid-renderer.md](plans/hybrid-renderer.md#forward-or-deferred)).

`sv::raster_routine` (rendering/raster_routine.hh) is the routine; `view_renderer` drives it from the same plan trace a traced layer goes through, before any accumulation or denoising.

## No ray tracing needed

**A raster layer draws on a device with no ray tracing at all.**
Ray tracing is sv's first choice and never its requirement ([the roadmap](plans/rendering-roadmap.md#decided-for-the-raster-path-and-the-hybrid)).
Nothing a raster frame records traces, and nothing it needs waits on something that does:

- **No acceleration structure is built.** A mesh is `complete` once its geometry lands, and the first trace reaching it builds its BLAS.
  That is `mesh_manager::acquire_blas`, so a mesh only ever rasterized never gets one.
  A raster frame resolves its scene without a BLAS per instance, and only a trace builds a TLAS.
- **The tracer is not a dependency.** `view_renderer` warms `pathtrace_routine` where the device has a ray-tracing pipeline, and never waits on it.
  So a raster layer draws as soon as its own pipelines are up.
- **No hit group compiles.** A permutation's hit group stays cold until a trace starts it; a raster frame compiles only the permutation's pixel stage.
- **The pipelines need no feature of it.** The raster stages list `raster.drawn`, `tracer.bindless` and `slug.tables`, and none of them holds an acceleration structure.
  Module `tracer`'s file-level `require raytracing_pipeline` binds only the stages that list a binding needing it.
  `tracer.bindless` does require `binding_arrays`, so a raster layer needs that feature, which WebGPU lacks.
- **The geometry buffers carry build-input usage only where the device builds structures**, since vulkan refuses it on a device without them.

A traced layer on such a device is declined, with one logged error naming the missing feature, rather than drawn blank: nothing it could draw is its image.
`raster-without-raytracing-test.cc` holds both on a dx12 context created with `withhold_raytracing`.

## Selecting it

**A third `render_mode`, not a separate setting.**
`render_settings::mode` already answers "how is this layer's image produced from its scene", already restarts the image when it changes, and is already folded into the trace hash.
A second setting would need a precedence rule — what does raster plus wavefront mean — and every reader of either would have to know it.
The hybrid joins as a fourth value of the same switch: rasterized primaries, traced lighting.

## Materials are the tracer's

**There is no second BSDF.**
The generated pixel stage opens with `material_body`: the `sv_supplied_*` constants, one `sv_attribute_*` load per attribute, and `sv_evaluate_material` around the type's own fragment.
That is byte for byte the text the hit group holds, with one exception:

- **A texture samples with the pixel's derivatives**, where a hit takes its ray cone's footprint axes as gradients; both filter anisotropically.

Both sample through the same sampler palette, which sits in `tracer.bindless` beside the tables rather than in either path's group 0.
A generated load names the group that holds its sampler, a raster pipeline binds no `tracer.traced`, and sg allows three groups, all of which a raster pipeline already lists.

Everything after the material is shared code too:

- `openpbr.shading_frame` builds the shading frame — authored frame, normal map, anisotropy, coat normal — for a hit and a pixel alike; it was lifted out of `tracer.shade_sampled` for this.
  It turns an authored frame to the side the turned geometric normal is on, and leans a normal the viewer is behind, or barely sees, toward the viewer.
- `openpbr.bsdf_prepare` and `openpbr.bsdf_eval` are the closure and its evaluation.
- `tracer.decal_reach` and `tracer.paint_decal_shape` paint the decals; `tracer.apply_decals` and `raster.apply_decals` are the two loops over them.
- The face normal is the triangle's own, from its corners, turned toward the viewer as `tracer.shade_triangle` turns it.

The pixel stage is compiled at run time through `sv::acquire_shader_library`, like the wavefront's shade kernels, and its pipeline is cached on the permutation's key.
One still compiling is drawn through the fallback permutation's pixel stage.

## Lighting

`raster.shade` lights a surface with what the tracer's next-event estimate converges to, evaluated rather than sampled, and **unshadowed**:

- **A point light, a spot and a parallel light are exact.**
- **A rect is integrated over a fixed 4×4 grid** of points on it, which is exact in the limit and close for a light not much nearer than its own size.
  The Cornell box test holds each block and channel to the tracer's direct light within 10% of the traced value plus 0.05, so a dark block is held to about 0.005.
- **A sun is its center direction**, carrying the whole solid angle the disc subtends.
- **The environment is an approximation**: its SH irradiance through the diffuse albedo, and its radiance about the mirror direction through the specular albedo.
  The second blurs toward the irradiance as the surface roughens.
  Nothing occludes it but baked ambient occlusion: where an asset has it, the type's `occlusion_attribute` darkens this term and no other.
  A closed room without it, lit by its environment, is lit inside as if open.
- **Emission** is the closure's own.

Nothing bounces, and nothing refracts: a glass sphere shows its reflection and nothing behind it.

## Cutouts, quadrics, decals and drawings

- **A cutout material cuts through the tracer's own reader.**
  The pixel stage reads the instance's opacity record through `tracer.instance_opacity`, the function the any-hit cuts through, ahead of the evaluation, and discards.
  It reads level 0 as a hit does: a box-filtered mip averages opacity, so a minified cutout would thin and vanish.
  A cutoff above 0 thresholds, as glTF's MASK, and there a pixel is cut exactly where a hit is.
  **Coverage is the one exception**: a cutoff of 0, which a tracer converges to a partial surface and one raster frame cannot, so coverage below one half is cut.
- **A quadric batch draws each primitive as its box**, and the pixel solves `quadric.intersect` along its camera ray and writes the surface's depth.
  A batch still streaming is drawn as its placeholder box through the fallback, as a pending mesh is and as a trace draws it.
  [quadrics.md](quadrics.md#the-raster-path) is the design: the boxes, the far faces, the conservative depth and the pass of their own.
- **Decals are painted** through the same functions the tracer paints with.
- **Drawings and annotations work**: the raster writes the primary hit's clip depth into the same slot a trace does, which the drawings' depth test reads.

## The G-buffer seam

**The seam is the layer's guide slots.**
Every frame writes, beside the image, the primary hit's G-buffer into the `temporal_id` slots the tracer writes its denoiser guides into, in the same formats and the same conventions:

| target | slot | format | holds |
|---|---|---|---|
| `color` | `accumulation` | rgba32_float | radiance, and its luminance squared in alpha, `pt.moment_of` |
| `normal` | `normal_guide` | rgba16_float | the geometric normal turned toward the viewer, world space |
| `depth` | `depth_guide` | r32_float | linear view depth along the camera's forward axis |
| `albedo` | `albedo_guide` | rgba16_float | `openpbr.pt_diffuse_albedo` |
| `specular_albedo` | `specular_albedo_guide` | rgba16_float | `openpbr.pt_specular_albedo` |
| `roughness` | `roughness_guide` | r16_float | `openpbr.pt_roughness` |
| `motion` | `motion_guide` | rg32_float | this pixel minus where its surface sat on the previous frame's image, in pixels |
| `primary_depth` | `primary_depth` | r32_float | clip depth, `1 - near / depth` |

A pixel no surface covers holds what an escaped primary ray leaves there.
That is 1 in `primary_depth`, the environment's reprojection in `motion`, and zero in every other G-buffer target.

What follows from putting it there:

- **A lighting pass reads a raster layer's primary hits exactly as it reads a traced one's**, by slot, so the hybrid needs no new plumbing to find them.
- **A denoiser could run on a raster layer unchanged**, which the hybrid's noisy traced lighting will want: the guides are already where it looks.
- **Every slot the renderer mints is a render target too**, so a layer switching between tracing and raster keeps its slots.
- `sv::raster_gbuffer` is the same set as a struct, for a caller driving `raster_routine` by hand.

What a hybrid lighting pass will want that is not here yet:

- **The material itself, rather than its albedos.** A traced bounce off a primary hit needs the closure, which the G-buffer does not carry.
  The hybrid answers it with a thin G-buffer of its own holding a reduced closure, rather than a visibility buffer ([hybrid-renderer.md](plans/hybrid-renderer.md#the-thin-g-buffer)).
  Its lighting pass then writes the guides, so a hybrid frame does not write these targets from the raster.
- **Object motion**, which needs scene items with an identity that survives a frame, as the tracer's motion does.

## Bindings

The pipelines list the tracer's groups in the tracer's order:

- group 0 is `raster.drawn`: the frame block, the instance table, the lights, the environment and the decals;
- group 1 is the manager's bindless tables and the sampler palette, `tracer.bindless`, bound as the very group a trace binds;
- group 2 is `slug.tables`, the decal atlas;

The records in `raster.drawn` are the tracer's own types, so a layer switching between the two uploads the same bytes.

## Not done yet

- **Shadows.** Deliberately not ray-query shadows: that is the hybrid's lighting pass, not raster's.
- **Antialiasing.** Every pixel is shaded at its center, so edges alias where the tracer's jitter antialiases them.
- **Blended transparency**: coverage is cut at one half, and transmission shows no refraction.
- **Coverage-preserving mips.** A cutout reads level 0, so a minified one keeps its coverage but aliases.
  The quality fix is mips generated to keep the fraction above the cutoff, with the cutout then sampled through them.
- **Lights seen by the camera**: a rect light marked visible to the camera is not drawn.
- **Upscaling**: a raster layer never draws at a scaled extent.
- **Light linking** is reserved in the records and read by neither path.
- **A vulkan rendering scope splits once per fresh upload**: a draw's bindings are barriered at the draw, inside the scope.
  sg has no way to record a draw's accesses before its scope opens, and that is the extension that removes the split; sg's TODO tracks it.

## What culling and the prepass measured

`raster-benchmark.cc` (`uv run dev.py benchmark "raster on Sponza"`) draws the same frames culled by nothing, by the view, and by occlusion too, without and with the depth prepass.
The contenders run interleaved frame by frame, timed on the GPU.
On Intel's Sponza, 405 draws over 4 permutations, at 1920x1080 on a laptop RTX 5070 in October 2026:

| culling | no prepass | prepass |
|---|---|---|
| none | 1.73 ms | |
| view | 1.48 ms | 1.02 ms |
| view and occlusion | 1.55 ms | 1.14 ms |

**The prepass pays even for raster's own cheap pixel stage**, a third of the frame on Sponza, so it is on by default.
A pixel stage that traces, the hybrid's, only raises what it saves.

**On Sponza occlusion culling costs more than it saves.**
Its fixed cost — the pyramid's levels, a second cull and a second rendering scope — is larger than the draws it removes from a scene of few large pieces.
Whether it pays on a scene of many small ones is what the benchmark's next scene decides.

## Tests

- `raster-test.cc`: the empty Cornell box against the megakernel's direct light, block by block, and its G-buffer at known points.
  Beside it: cutouts thresholded and as coverage, a capture of a raster layer finishing on its first drawn frame, and vulkan against dx12 to rounding.
  Two planes a unit apart at 2000, which a standard float depth cannot tell apart, hold the reversed depth test.
  A frame drawn with and without the depth prepass is the same bit for bit, culled by the view and by occlusion.
  A textured cutout minified until its mips fall under the cutoff holds the cut to level 0.
  A baked occlusion of one half halves what the environment lights.
- `material-shader-generator-test.cc`: the raster spelling of a cutout material, cut through `raster.is_cut`.
- `material-shader-compile-test.cc`: every generated pixel stage compiles, beside its hit group and shade kernel.
- `render-plan-test.cc`: the slots a raster layer declares.
- `raster-without-raytracing-test.cc`: a raster layer's capture on a device without ray tracing, and a traced layer there declined with one error; neither builds an acceleration structure.
- The `shaped-viewer/raster` example draws one scene traced and rasterized side by side.
