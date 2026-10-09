# The raster path

**Built, and a choice rather than the default.**
A layer whose `render_settings::mode` is `render_mode::raster` is drawn by sg raster pipelines instead of traced: the same scene, the same OpenPBR materials, lit directly.
It is raster alone for now, and it is shaped as the base the interactive hybrid mode grows from.
That mode is rasterized primary visibility, then ray-traced lighting resampled by ReSTIR GI ([restir-gi.md](plans/restir-gi.md)).
This document is what a raster frame draws, how it shares its materials with the tracer, the G-buffer seam a hybrid lighting pass reads, and what is not done yet.

## What a frame is

```
one draw per triangle item ─▶ environment where nothing was drawn
   (vertex: pull the corner, project)      (depth test against the far plane)
   (pixel: the material, then raster.shade)
```

- **One draw per scene item**, three vertices per triangle and no vertex or index buffer.
  The vertex stage, `surface_vs` of [raster_pass.sgl](../shaders/raster_pass.sgl), pulls its corner out of the bindless tables the way a hit reads its triangle, through the instance record.
  So one vertex stage serves every mesh, indexed or not, whatever attributes it carries.
- **The pixel stage is generated per material permutation**, `raster_source` beside the hit group and the wavefront's shade kernel.
  It builds the hit's `material.shading_context` from the triangle and its barycentrics, evaluates the material, and hands the surface to `raster.shade`.
- **The environment** is a fullscreen pass on the far plane, depth-tested `less_equal`, so it lands only where no surface was drawn.
- **Every frame is final.**
  The layer's accumulation slot is set to `accumulation_frame_cap` the frame it lands, so `view_ref::is_accumulation_converged` holds at once and a capture finishes on its first drawn frame.
  A frame drawn through a stand-in material is not final: it restarts next frame, as a trace with a stand-in does.
- **Nothing denoises.** `render_settings::reconstruct` is ignored, and neither the denoised slot nor the frame-samples slots are declared.

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
- **The geometry buffers carry build-input usage only where the device builds structures**, since vulkan refuses it on a device without them.

A traced layer on such a device is declined, with one logged error naming the missing feature, rather than drawn blank: nothing it could draw is its image.
`raster-without-raytracing-test.cc` holds both on a dx12 context created with `withhold_raytracing`.

## Selecting it

**A third `render_mode`, not a separate setting.**
`render_settings::mode` already answers "how is this layer's image produced from its scene", already restarts the image when it changes, and is already folded into the trace hash.
A second setting would need a precedence rule — what does raster plus wavefront mean — and every reader of either would have to know it.
The hybrid joins as a fourth value of the same switch: rasterized primaries, traced lighting.

The enum was `path_integrator` while it held the two tracers alone, and is named for what it chooses now that it holds more than paths.

## Materials are the tracer's

**There is no second BSDF.**
The generated pixel stage opens with `material_body`: the `sv_supplied_*` constants, one `sv_attribute_*` load per attribute, and `sv_evaluate_material` around the type's own fragment.
That is byte for byte the text the hit group holds, with two exceptions:

- **The sampler palette lives in `raster.drawn`**, a copy of `tracer.traced`'s, since a generated load names the group that holds its sampler and a raster pipeline binds no acceleration structure.
- **A texture samples at the level the pixel's derivatives pick**, where a hit names level 0.

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
  The Cornell box test holds it to the tracer's direct light to within 4% per block.
- **A sun is its center direction**, carrying the whole solid angle the disc subtends.
- **The environment is an approximation**: its SH irradiance through the diffuse albedo, and its radiance about the mirror direction through the specular albedo.
  The second blurs toward the irradiance as the surface roughens.
  Nothing occludes it, so a closed room lit by its environment is lit inside as if open.
- **Emission** is the closure's own.

Nothing bounces, and nothing refracts: a glass sphere shows its reflection and nothing behind it.

## Cutouts, quadrics, decals and drawings

- **A cutout material cuts what the tracer's any-hit would.**
  The pixel stage reads the type's declared opacity and cutoff through the material's own attribute loads, ahead of the evaluation, and discards.
  A cutoff above 0 thresholds, as glTF's MASK; a cutoff of 0 is coverage, which a tracer converges to a partial surface and one raster frame cannot, so coverage below one half is cut.
- **Quadric batches are not drawn.** The layer logs a warning once and draws its meshes.
  The plan is a pixel stage that ray-casts `quadric.intersect` over each primitive's box and writes its own depth.
  The intersection and the shading context already exist as functions of module `quadric`.
- **Decals are painted** through the same functions the tracer paints with.
- **Drawings and annotations work**: the raster writes the primary hit's clip depth into the same slot a trace does, which the drawings' depth test reads.

## The G-buffer seam

**The seam is the layer's guide slots.**
Every frame writes, beside the image, the primary hit's G-buffer into the `temporal_id` slots the tracer writes its denoiser guides into, in the same formats and the same conventions:

| target | slot | format | holds |
|---|---|---|---|
| `color` | `accumulation` | rgba32_float | radiance, alpha 1 |
| `normal` | `normal_guide` | rgba16_float | the geometric normal turned toward the viewer, world space |
| `depth` | `depth_guide` | r32_float | linear view depth along the camera's forward axis |
| `albedo` | `albedo_guide` | rgba16_float | `openpbr.pt_diffuse_albedo` |
| `specular_albedo` | `specular_albedo_guide` | rgba16_float | `openpbr.pt_specular_albedo` |
| `roughness` | `roughness_guide` | r16_float | `openpbr.pt_roughness` |
| `motion` | `motion_guide` | rg32_float | this pixel minus where its surface sat on the previous frame's image, in pixels |
| `primary_depth` | `primary_depth` | r32_float | clip depth, `1 - near / depth` |

A pixel no surface covers holds zero in every G-buffer target and 1 in `primary_depth`, which is what an escaped primary ray leaves there.

What follows from putting it there:

- **A lighting pass reads a raster layer's primary hits exactly as it reads a traced one's**, by slot, so the hybrid needs no new plumbing to find them.
- **A denoiser could run on a raster layer unchanged**, which the hybrid's noisy traced lighting will want: the guides are already where it looks.
- **Every slot the renderer mints is a render target too**, so a layer switching between tracing and raster keeps its slots.
- `sv::raster_gbuffer` is the same set as a struct, for a caller driving `raster_routine` by hand.

What a hybrid lighting pass will want that is not here yet:

- **The material itself, rather than its albedos.** A traced bounce off a primary hit needs the closure, which the G-buffer does not carry.
  The likely shape is a visibility buffer: instance and primitive per pixel, with the barycentrics recomputed.
  The lighting pass then re-evaluates the material through the same generated body, sorted per permutation as the wavefront's shade stage is.
  That is two `uint`s per pixel added to the targets, and the reason `raster.varyings` already carries both.
- **Object motion**, which needs scene items with an identity that survives a frame, as the tracer's motion does.

## Bindings

The pipelines list the tracer's groups in the tracer's order:

- group 0 is `raster.drawn`: the frame block, the instance table, the lights, the environment, the decals, and the sampler palette;
- group 1 is the manager's bindless tables, `tracer.bindless`, bound as the very group a trace binds;
- group 2 is `slug.tables`, the decal atlas;
- the inline constants are `raster.draw`, the one row of the instance table a draw is.

The records in `raster.drawn` are the tracer's own types, so a layer switching between the two uploads the same bytes.

## Not done yet

- **Shadows.** Deliberately not ray-query shadows: that is the hybrid's lighting pass, not raster's.
- **Quadric batches**, above.
- **Antialiasing.** Every pixel is shaded at its center, so edges alias where the tracer's jitter antialiases them.
- **Blended transparency**: coverage is cut at one half, and transmission shows no refraction.
- **Lights seen by the camera**: a rect light marked visible to the camera is not drawn.
- **Overdraw**: there is no depth prepass, so an occluded surface is shaded before the one in front of it lands.
- **Vertex reuse**: the vertex stage runs three times per triangle, since no index buffer is bound.
- **Upscaling**: a raster layer never draws at a scaled extent.
- **Light linking** is reserved in the records and read by neither path.
- **A vulkan rendering scope splits once per fresh upload**: a draw's bindings are barriered at the draw, inside the scope.
  sg has no way to record a draw's accesses before its scope opens, and that is the extension that removes the split; sg's TODO tracks it.

## Tests

- `raster-test.cc`: the empty Cornell box against the megakernel's direct light, block by block, and its G-buffer at known points.
  Beside it: cutouts thresholded and as coverage, a capture of a raster layer finishing on its first drawn frame, and vulkan against dx12 to rounding.
- `material-shader-generator-test.cc`: the raster spelling of a textured cutout material.
- `render-plan-test.cc`: the slots a raster layer declares.
- `raster-without-raytracing-test.cc`: a raster layer's capture on a device without ray tracing, and a traced layer there declined with one error; neither builds an acceleration structure.
- The `shaped-viewer/raster` example draws one scene traced and rasterized side by side.
