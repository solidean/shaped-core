# Lights

How sv describes the lights in a scene, and why it is factored the way it is.
The record lives in [scene/light.hh](../src/shaped-viewer/scene/light.hh); the authoring calls are on `scene_ref` in [refs.hh](../src/shaped-viewer/refs.hh).

**Every path is traced: points and spots, rects with either face or both, parallel light and suns.**
A sun is always a light, never part of a background.
Every step of the landing order is in; what is designed and not landed is below.

## What a light is

```cpp
scene.add_point_light("bulb", tg::pos3f(0, 3, 0)).candela(800);
scene.add_spot_light("key", tg::pos3f(0, 3, 0), tg::vec3f(0, -1, 0), 25_deg_f).candela(800).color(warm);
scene.add_rect_light("softbox", center, half_u, half_v).nits(12).spread(20_deg_f);
scene.add_sun_light("sun", tg::vec3f(-0.3f, -1, -0.2f)).lux(110'000);

// the same through the record, with the factory and the setters on the light itself
scene.add_light("key", sv::light::spot(p, down, 25_deg_f).candela(800));
```

The `shaped-viewer/lights` example puts every kind side by side over one stage — [lights.cc](../examples/lights.cc).
`shaped-viewer/light-stage` puts colored lights of every kind into one scene at once — [light-stage.cc](../examples/light-stage.cc).

`sv::light` is **one record whatever the kind of light**, tagged by the path the integrator takes to sample it.
Fixture factories — `point`, `spot`, `rect`, `directional`, `sun` — pick the path and fill its payload.
Chained setters fill the rest, and `scene_ref` has a sugar call per factory that returns a `light_ref` taking the same setters.

It is one record rather than a type per kind because nothing that iterates lights wants only some of them.
The flatten, the GPU upload, the trace hash, a UI and an importer all want every light.
With a type per kind, each of them grows a branch per kind.
`sv::quadric_primitive` settled the same question the same way: one record, named factories.

## The tag is a path, not a shape

`light_path` enumerates the branches the integrator has, because that is the grouping that matters: lights on one path are sampled, weighted and intersected alike, and the GPU buffer is grouped by it.
A geometric list would merge paths that differ and split paths that do not.

| path | what it is | sampled as | falloff | landed |
|---|---|---|---|---|
| `point` | a point source; a spot is one with a cone | delta, one direction | inverse square | traced |
| `area` | a flat patch, `rect` today | a point on the patch | inverse square | traced |
| `distant_point` | parallel light | delta, one direction | none | traced |
| `distant_disc` | the sun, with an angular size | within the cone it subtends | none | traced |
| `sphere` | a bulb with a radius | within the cap it subtends | inverse square | designed |
| `mesh` | an emissive instance, appended by the renderer rather than an `sv::light_path` value | a point on one of its triangles | inverse square | traced |

A delta light is never reached by a sampled ray, so it takes no multiple importance sampling at all — that is a different branch, not a degenerate size.
So a size of zero is normalized away by the factory rather than stored: `sun(dir, 0)` is a `distant_point`, and a zero-radius sphere will be a `point`.

A disc is an `area` light with a different in-region test, which is why `area_shape` exists with one value.

## Placement

A light is placed by a `tg::similarity_transform3f`: moved, turned and uniformly scaled, never sheared.
A shear would turn a sphere into an ellipsoid and a cone into something no sampler handles, so it is inexpressible rather than rejected.
The size lives in the payload, in the light's own frame.

**A light points along its placement's -Z**, as glTF's punctual lights do, so an importer copies a node's transform instead of correcting it.
That is a spot's axis, a rect's front face, and the direction a distant light's light travels.

## Brightness and units

A light's brightness is a unitless `color`, a scalar `intensity`, an explicit `light_unit`, and an `exposure` in stops on top.

| unit | quantity | accepted by |
|---|---|---|
| `candela` | luminous intensity, lm/sr | `point`, `area` (along its normal) |
| `lux` | illuminance, lm/m² | `distant_point`, `distant_disc` |
| `nit` | luminance, cd/m² | `area` |
| `lumen` | the whole output | `point`, `area` |

A unit its path cannot mean asserts where it is written, not where it is traced.

**One nit is one unit of the tracer's radiance.**
That is also what OpenPBR's surface `emission_luminance` means, so an emissive surface and an area light of equal luminance give equal radiance.

The unit is what answers the resize question.
In nits a bigger light is brighter; in lumens it is softer instead, since the same flux is spread over more area.
There is no separate `normalize` flag because the unit already says which one was meant.

Units are resolved when a light is laid out for the GPU, so the shader sees one quantity per path and never a unit.
An area light converts through its emitting area A: a Lambertian face of radiance L emits `pi * A * L` lumens, and `L * A` candela along its normal.

**Lumens on a shaped light count as if it were unshaped**, Blender's convention.
A spot's lumens are divided by `4 pi` whatever its cone, and a spread rect's by `pi * A` per face whatever its spread.
So a spot's lumens are the whole bulb's behind the cone, and narrowing either light makes it no brighter.

## Shaping and faces

A cone is not a kind of light, it is a *shaping* on one.
A spot is a point with a cone, and an area light's spread is the same cone with equal angles — a softbox with a grid over it.
The cone is stored as glTF stores it, inner and outer half-angles about -Z, and falls off by glTF's formula, so an imported spot looks as it did where it was authored.

A cone on a distant light asserts: it would be a barn door, and nothing implements one.

An area light chooses which face emits, `front`, `back` or `both`.
An emitter whose edges emit too is not a flag on a rect — a rect has no edges with area — but a `box` shape, which is designed and not landed.

## Identity

`add_light` takes an id, which is what the light is known by across frames.
It is a `light_id` — `stable_id<light_id_tag>`, the same template a `view_id` is — hashed from the string under the frame's id stack.
So a loop of lights is scoped exactly as a loop of views is: `f.scoped_id(i)` around the body, or a `##i` suffix.

Two lights with one id in one scene layer assert, since whatever the renderer keeps per light would be shared.
The same id in another layer is a different light.

A `light_ref` indexes the caller's list, in the order the lights were added, never the GPU buffer, which the flatten is free to reorder.

Per-light state kept across frames is keyed on the id, and a change of path invalidates it as a vanished id does.
Nothing keeps such state yet; the id is taken now because it is one argument while there are few call sites.

## Validity

Factories are the only way to build a light, and each leaves it valid.
A setter applies its change to a copy and commits it only once `light_problem` finds nothing, so a setter that asserts leaves the light as it was.
`placement`, `emission` and `shaping` stay public, so a direct write can still make a light invalid.
`light_record_of` asserts `light_problem` too, so whichever way a light was built, an invalid one never reaches the tracer.
`light_problem` is public, for a caller holding a light from elsewhere.

## On the GPU

`sv::light_record_of` fills one tagged 96-byte `tracer.light_record` per light, the type slib generates from `scene.light` in shaders/sgl/scene_items.sgl.
Its `emission` is one canonical quantity per path: a point's intensity, a rect's radiance per face, a parallel light's irradiance, a sun's radiance.
A sun's radiance is its illuminance over `pi * sin(r)^2`, the projected solid angle of a disc of angular radius r, so a surface facing it receives exactly the lux it was given.
Its size is stored as `1 - cos(r)` rather than as a cosine, which near 1 would round away most of a small disc, and the shader's sampler, density and in-disc test all work in that quantity.
A cone is stored as the scale and offset of glTF's falloff, which an unshaped light turns into a constant 1, so no path branches on whether it has one.
The flatten groups them by path with `pt_light_table` — a counting sort over the walk it does anyway — and the frame block carries the count and each path's offset and length.
A loop over one path's run, like the bounce ray testing every area light, then runs in one branch.

Next-event estimation picks one light with the probability its record carries, `select_pdf`, which `pt_light_table::grouped` writes.
That probability is inside `pt.light_pdf`, `pt.disc_pdf` for a sun, and `pt.mesh_light_pdf` for an emissive instance.
That density is the one thing both estimators share, since the next-event sample and the bounce ray reaching a light weight against each other.
A density they disagree about makes the image wrong rather than noisy.
With exactly one light no pick is drawn, so a one-light scene keeps the sample sequence it always had.

A point or a parallel light is a delta that no sampled ray reaches, so it has next-event estimation alone and is divided by the pick probability only.
On a path's last surface hit no bounce ray follows, so next-event estimation takes the full weight there for every light and the sky;
weighting it against a ray that never flies would darken every rect and sun by the share left to that ray.
A rect and a sun have both strategies: the bounce ray credits every rect it crosses and every sun it escapes into, each weighted against its own density.
A bounce ray that left by transmission, or after a medium scatter, takes the full weight instead.
Next-event estimation samples only directions above both the shading frame and the geometric face, and a medium scatter takes no sample.

Lights occlude nothing — a shadow ray toward one passes straight through another — so the bounce ray credits every area light it crosses before the surface, not only the nearest.
Treating lights as transparent on one side and opaque on the other would leave part of the integral to neither strategy.

Two tests pin all of this.
`sv - every kind of light traces to the sum of each alone` traces one light of each path alone and all four together.
Light is additive, and dropping the pick from a density puts the sum off by a clear factor.
`sv - every kind of light delivers the illuminance its unit promises` puts each path above a floor at the same illuminance and asks the floor to agree.
That pins every unit conversion, at one bounce, where next-event estimation carries it all.
`sv - a sun delivers its illuminance with its bounce-ray strategy in play` repeats the sun at two bounces against a parallel light.
The parallel light has no bounce branch, so an error in the sun's bounce weighting shows there instead of cancelling.

A layer with no lights of its own is traced under `layer::fallback_light`, a sun unless the layer sets another or turns it off.
A traced layer whose only lights are emissive instances counts as lit, and takes no fallback.
A raster layer draws no emitter's light, so it keeps its fallback.

## Emissive instances

**An emissive instance is a light of the trace, but no `sv::light` makes it.**
The renderer appends one record of path `scene.light_path_mesh` (`sv::light_path_mesh`, 4) per qualifying instance, after every analytic light.
So emitters and analytic lights share one selection CDF, and a frame's work grows with emissive instances, never with triangles.
[plans/emissive-triangles.md](plans/emissive-triangles.md) is the design and its phasing.

**Only a type that declares its emission lights the scene.**
`material_type::emission_color_attribute` (a float3) and the optional `emission_scale_attribute` (a float) say where it comes from.
The radiance is `max(0, scale) * max(0, color)`, and the generator writes it into `surface` after the fragment, so the closest hit and the light sampler read one value.
openpbr declares `emission_color` and `emission_luminance`, pbr declares `emissive` at a scale of 1, and unlit declares none: it is a display material, which the camera sees and nothing else does.

**An instance qualifies** where its own permutation declares an emission that is not dark wherever it is read.
Each factor may be a constant, a texture or a mesh attribute, per vertex, per corner or per triangle.
Dark means a constant at or below zero, or a texture or an attribute whose every value is, which its summary proves only where it read them all.
A texture's lane under a transform scale of exactly 0 reads its bias whatever the texels hold, so that lane is known without them.
So the test Cornell box's walls, whose per-triangle `emissive` is zero throughout, light nothing.
A constant or a texture transform that is not finite makes no light either, since no estimate could weigh it.
It must also be a resident triangle mesh, shaded by that permutation rather than by the fallback a streaming mesh takes.
`sv::impl::emitter_estimate_of` (rendering/impl/emitters.hh) is the material half of that rule; a quadric batch is camera-only.

**An instance that does not qualify is camera-only**: the camera sees its emission, and no bounce meets it as a light, since `tracer.emitter_pdf` finds no record for it.
So exclusion must be proven rather than guessed, which is why only an exact summary may exclude.
A summary is exact only where it read every value the sampler reads: the base level, whole, uncompressed.
A texture the manager re-encodes to BC7 keeps the summary of its source pixels, made inexact, since the encoding's error may lift a texel off zero.
**A texture's summary counts only once the texture samples its own texels**, at `base_resident` or `complete` (`sv::impl::sampled_texture_summary`).
A pending texture samples a placeholder and a failed one magenta, so either is unknown, whatever its summary proved, and qualifies at an unknown 1.
A dark texture thus lights nothing from the frame its upload lands.
Under a bias that is not zero, a texture's lane must stay a margin below zero to be dark, since the GPU's filtering, sRGB decode and fused multiply-add round otherwise than the CPU does.

**Its record** carries an estimate of the radiance as `emission`, two-sided, and its world area, and only selection reads either.
Under a similarity the area is the object area times `|det M|^(2/3)`, which is exact there.
Under any other placement that undercounts a flat lamp flattened along its normal, to nothing at a zero scale, so the GPU sums the exact area instead (`mesh_manager::acquire_world_area`).
The estimate stands in until that lands a frame or two later, and the landing restarts the image once, as the CDF's does.
Its weight is that area times π times two faces times the luminance, beside the local lights.
The estimate takes each factor's mean: a constant's value, a texture's through its swizzle and its scale and bias, an attribute's over its elements.
A texture's and an attribute's means are `sv::value_summary`s, read on the CPU at the acquire that uploads them, while the payload is at hand.
A texture's comes from the smallest level the acquire supplied, read whole up to 65536 texels and in short row runs on a coarser grid beyond.
An attribute's is read whole up to 65536 elements and in short runs beyond, so a larger one is never proven dark.
An 8-bit sRGB texel decodes through a 256-entry table, so the scan costs a table lookup per channel.
Every texture is summarized, since which ones a material reads as an emission is not known at the acquire.
A block-compressed level is decoded by `babel::bcn`, which estimates the mean, and its summary is never exact.
A format it cannot decode has none, and reads as 1 before the factor's swizzle and its scale and bias.
Where the means multiply to zero, each factor's bound stands in, exact or not, else its mean, so an emitter that can emit is always picked sometimes.
A factor no read saw above zero, as a strided scan that met only black, stands in as a tiny value above zero, and its record carries `sv::light_flag_unseen_emission`.
Such a record is no evidence of light: beside a lit sun, local lights made only of those take a thousandth of the choice rather than half of it.
Among other local lights its weight is the light table's floor, a thousandth of the mean of its kind, so an importer's black emission map costs a scene little.
The estimate is finite, and so is each light's selection weight: a NaN weight reads as 0 and an overflowing one as the largest f32.
The weight shapes the noise and never the mean.
`traced.emitters` holds the rest, one `emitter_record` per mesh record from `frame_constants::emitter_offset`: the instance's row, its mesh's area CDF, and where each factor of its emission lives.
The instance record's `emitter` names the light record back, so a bounce ray that meets the instance finds its density.

**The area CDF is the mesh's, built on the GPU** by shaders/emitter_cdf.sgl the first time a trace needs it, and cached with the mesh as its BLAS is (`mesh_manager::acquire_emitter_cdf`).
It is two-level — triangles within blocks of 256, then blocks — so the f32 widths stay accurate over millions of triangles, and a zero-area triangle has a width of exactly 0.
It is in object space, which is unbiased under any placement: the density a sample carries is its triangle's width over the triangle's world area.
While its kernels compile an instance is camera-only, and the frame its CDF lands restarts the image.

**Next-event estimation** picks a triangle by the CDF and a uniform point on it.
Its solid-angle density, `pt.mesh_light_pdf`, is the record's pick probability times the triangle's width over its world area, times the squared distance over the cosine at the light.
The emission at the point is read from data by one reader for every material, `tracer.emitter_radiance`, and scaled by the instance's cutout opacity there.
It reads a constant out of the parameter block, interpolates an attribute at the point, and samples a texture through its uv, palette sampler, swizzle and transform.
**A texture is sampled at level 0 on both sides**: a closest hit and a shade kernel have no derivatives, so the generated material samples level 0 too.
The two strategies weighed against each other thus read one value, which the balance heuristic needs.
**A bounce ray that meets an emitter** forms the same density at its hit (`tracer.emitter_pdf`) and weighs the emission by the balance heuristic; the camera's own view keeps its full weight.

## Landing order

1. **N lights on the GPU.** Landed.
2. **The new paths.** Landed: `point` with its cone, `distant_point`, `distant_disc`, two-sided faces, spread, and a sun as the fallback.
3. **Sun ownership.** Landed.
   `background::sun` is now `background::lobe`, named for what it is — a soft fill that casts no shadow.
   `sv::daylight()` returns a `sky_and_sun`: a sky with no lobe in it, and a `distant_disc` light as bright as the lobe it replaced.
4. **Units.** Landed.
   `sv - a surface's emission and an area light's nits are the same radiance` pins one nit as one unit of the tracer's radiance on both sides.
   glTF's `emissiveFactor` has no physical unit, so the importer reads it one-to-one as nits, times `KHR_materials_emissive_strength` once babel reads that.
5. **Import.** Landed.
   babel's glTF reader interprets `KHR_lights_punctual` — its first interpreted extension — and `asset_data` gains a flat, world-placed `lights` list.
   What the importer cannot represent becomes an entry in `asset_data::issues`, and nothing new fails a load.
   A light takes only a position and a direction from its node, since the extension says the node's scale does not affect it, and glTF's `range` is stored and ignored, since it is a hint.
   Unflattened, each `asset_node` names the run of lights it placed, as it does for meshes.
6. **Non-physical flags.** Landed.
   `visible_to_camera`, off by default and only for a light with an extent, and `casts_shadows`, on by default.
   A light that casts no shadow is ignored by geometry on both sides of the weighting, or the two would disagree about what they integrate.
   A light-linking mask is reserved in both GPU records, light and instance, all ones and read by nothing.
7. **Power-weighted selection, mixed with uniform.** Landed, in both tracers.
   `pt_light_table::grouped` writes each record's `select_pdf` and the running `select_cdf`, and next-event estimation picks by a binary search over them.
   Half of the choice is by power: local lights are weighed by power and distant ones by irradiance, which do not compare, so the two kinds share that half and half where both have light.
   The other half is uniform over the lights with light to offer, since power knows nothing of distance: alone it starved a dim light that is the near one somewhere.
   light-stage's bulb by its stairs was twice as noisy by power alone as by uniform selection; the mixture's cost and gain are pinned by a wavefront test with a dim corner light.
   An emissive instance whose emission no read saw above zero is no light for that rule: it takes none of the uniform half, and only keeps its kind a thousandth of the power half.
   Every weight is floored at a thousandth of its kind's mean, so no light the multiple-importance weights count on is impossible to pick.
   A table of `light_selection::uniform` writes both 0, which the tracer reads as every light alike.
   A record no table wrote says the same.

## Designed, not landed

- The `sphere` path, and a `disc` area shape.
- Tube and box emitters.
- Gobos and IES profiles, as further shaping kinds.
  When a two-dimensional texture sampler is built, it is built once for the environment map and shared.
- The point at which emitters should move into the acceleration structure.
- **An HDR sky, and the sun inside it.** A captured map carries its own sun, while "a sun is always a light" says the sky holds none.
  Two rules fit, and which one is decided when the HDR sky lands:
  - the sun is extracted into a `distant_disc` light and clipped out of the map, keeping the rule as it stands;
  - the map is an importance-sampled light of its own and no sun light is added.
    Then it is open whether it stays the separate estimator every hit takes today, beside the picked light, or becomes a path of its own in the pick.
- A stable id per scene item, which picking will want; it is `stable_id<Tag>` again.
- **Procedural emission that lights the scene.** Emission a fragment computes reaches the camera alone, since next-event estimation reads only the declared attributes.
  The way there is to run the material's own fragment at a light sample, as a callable, at a material evaluation per shadow ray for every emissive type.
  Baking each triangle's emission to a constant instead is ruled out: a textured emitter's light sample would then disagree with what a bounce ray sees, which biases the image.
- **An emitter's power measured on the GPU.** The world area is summed there already for a placement that is not a similarity.
  A texture's mean is still the CPU's summary, through a decoder for a compressed level; the kernel that walks the triangles could sum area times sampled emission instead.
  The light table is built on the CPU, so that result lands a frame or two late, which costs noise in that frame rather than an error that lasts.

## Lower-library gaps

- **typed-geometry has no orthonormal-basis routine.**
  Building a light's frame from a direction, and a tangent frame from a normal, both hand-roll one (`frame_with_z` in light.cc, `any_perpendicular` in asset/impl/tangent_frames.cc).
- **typed-geometry types are not hashable.**
  A CPU hash of a light would have to fold `tg::vec3f`, angles and transforms by hand.
  Nothing needs one yet: the trace hash covers the uploaded bytes, which is where equal lights must hash equal.
- **typed-geometry cannot classify a linear map.**
  Whether a placement is a similarity, under which an emitter's world area follows from its determinant, is `sv::impl::is_similarity` until it can.
