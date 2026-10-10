# Emissive triangles as lights

**A plan, built: phases P1, P2 and P3 have landed.**
An emissive surface used to light nothing but the camera; since P1 an instance whose emission is a constant is a light of the trace.
Since P2 one whose emission is a texture or a mesh attribute is too, its emission read at the sampled point from data.
Since P3 ReSTIR DI resamples points on them as it does points on rects.
This plan makes emissive triangles lights, sampled by next-event estimation and balanced against the bounce ray, in the megakernel, the wavefront and ReSTIR DI.

## Decisions

Settled by the user:

1. **This push lands P1 through P3**: constant and textured emitters, in both tracers, and ReSTIR DI over triangle samples.
2. **The test Cornell box keeps its emissive lamp quad and drops the matching rect**, so the lamp is the box's only light and every view-level test exercises emitter sampling.
3. **`unlit` stays camera-only.** It is a display material, and only a type that declares its emission attributes lights the scene.

## How it works

**A material type declares where its emission comes from.**
A type names an `emission_color_attribute` (float3) and an optional `emission_scale_attribute` (float), as it names an `opacity_attribute`.
Its emitted radiance is then `max(0, scale) * max(0, color)`.
The generator writes those two into the surface after the fragment, and a type that declares them may not write `surface.emission_*` itself.
So the closest hit and the light sampler read one emission, which the balance heuristic needs: two different answers would make the weights not sum to one.
openpbr declares (`emission_color`, `emission_luminance`), pbr declares (`emissive`, none), and unlit declares none.

**An emissive instance is one light record.**
An instance qualifies where its permutation declares emission that is not dark wherever it is read: each factor a constant, a texture or a mesh attribute.
It must also be a resident triangle mesh shaded by its own permutation rather than the fallback.
Each such instance appends one record of path `light_path_mesh` to the trace's light table, after the analytic lights.
So emitters and analytic lights share one selection CDF, and a frame's work grows with emissive instances, never with triangles.
Its selection weight is its power, 2π times its world area times its estimated emitted luminance, as a two-sided rect's is, and it joins the local half of the power-weighted choice.

**An emissive mesh carries an area CDF over its triangles.**
It is built on the GPU from the mesh's own buffers, the first time a trace needs it, and cached with the mesh as its BLAS is.
It is two-level, blocks of triangles and triangles within a block, so the f32 widths stay accurate over millions of triangles.
It is in object space, which is unbiased under any placement, since the pdf a sample carries is always its triangle's width over its world area.

**Next-event estimation samples a point on a triangle.**
A mesh record picks a triangle from its instance's CDF and a uniform point on it.
Its solid-angle pdf is the record's selection probability times the triangle's width, over its world area, times the squared distance over the cosine at the light.
Emitters are two-sided.
The emission at the point is read from data by one fixed reader, as cutout opacity already is, and scaled by the opacity there.
That is what lets the wavefront and ReSTIR DI evaluate a sample with no material code.

**A bounce ray that hits an emitter is weighted against it.**
The hit computes the light-sampling pdf of the point it hit from the same table, and the tracer weights its emission by the balance heuristic.
The camera's own view of an emitter keeps its full weight, and an emitter outside the table stays camera-only.

## Phasing

- **P1, landed**: the type contract, constant factors, the per-mesh area CDF, instance records, sampling and the bounce-ray weight in both tracers.
  ReSTIR GI routes a weighted emitter at the visible point's first bounce into the direct light, and ReSTIR DI left mesh samples to the path until P3.
- **P2, landed**: texture and mesh-attribute emission through the reader, and a per-texture average luminance for selection.
  The average is read on the CPU at the acquire, from the smallest level supplied, since the pixels are at hand there and a GPU readback would land frames later.
  An attribute is weighed by the mean of its elements, read there too.
  A texture or an attribute read whole and dark throughout makes no light, as a constant at or below zero makes none.
- **P3, landed**: ReSTIR DI over triangle samples: a reservoir holds an instance's record and a point on one of its triangles, and history is dropped when the table's indices change.
  A candidate draws through the draw next-event estimation makes, and its point is kept as the triangle and its barycentrics, so it is placed through the instance anew wherever it is read.
  The table's key is `pt_light_table::key`, which DI's history is kept under.
  The key covers every emitter's placement, so an emitter that moves drops DI's history in the wavefront routine, as the view's scene hash drops it; [restir-gi.md](restir-gi.md#di-track) has the rest.
- **Later**: a power-weighted triangle CDF for mostly-dark textured emitters, quadric emitters, one-sided emitters, and an alias table if profiling asks for it.

## Tests

Artificial scenes, in both tracers:

- An emissive quad lights a floor as a rect light of the same size and luminance does, at one bounce and at several.
- The weights sum to one: the mean is the same with emitters sampled and with them reached almost only by bounce rays.
- An emissive furnace: a closed box whose walls emit and reflect converges to its geometric series.
- An instance placed with a non-uniform scale and a rotation lights as the same mesh transformed in advance.
- Power selection over a dim large and a bright small emitter keeps the mean: both together light as the sum of each alone.
- A cutout emitter at opacity one half delivers half the light.
- The area CDF is monotone, ends at one, and gives a zero-area triangle zero width.
- An emitter of many uneven triangles, spanning three blocks of the CDF, lights as a rect of its size does.
- P2: a half-black, half-white textured emitter lights as a rect over its white half, and the camera sees the texture.
- P2: a per-vertex emitter's mean is the same sampled or reached by bounce rays almost alone, and the Cornell box's per-triangle lamp lights as a constant one does.
- P2: an indexed emitter reads a per-vertex and a per-corner ramp at the indexed vertex, the per-vertex one unbiased against bounce rays and the per-corner one lighting alike.
- P2: a per-triangle lamp of two different triangles lights as two constant ones do.
- P2: a textured color under a scale and a bias, times a textured luminance, lights as its constant halves do.
- P2, on the CPU: an exact summary alone excludes an emitter, a block-compressed level's summary is inexact, and a non-finite factor makes no light.
- P3: the 22-lamp ReSTIR DI test with the lamps as emissive fans keeps the plain mean and cuts its noise, away from the pixels that see a lamp.
- P3: rect lamps and emissive fans of unequal triangles, sizes and brightnesses, alternating in one table, keep the plain mean under ReSTIR DI.
- P3: a large emissive lamp seen in a glossy floor keeps the plain mean, which a re-shade weight of 1 would not.
- P3: an emissive lamp stretched to twice its area every other frame keeps each placement's mean with GI reuse alone, and reuses no DI history across the change.
- P3, on the CPU: the table's key follows what its indices name and where its emitters stand, but not selection or a bindless index.
  DI's history needs the key it was kept under and survives a camera move, and the DI passes compile.
