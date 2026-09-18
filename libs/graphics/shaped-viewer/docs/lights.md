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

// the same through the record, with the path-independent half as a designated initializer
scene.add_light("key", sv::light::spot(p, down, 25_deg_f), {.intensity = 800, .unit = sv::light_unit::candela});
```

The `shaped-viewer/lights` example puts every kind side by side over one stage — [lights.cc](../examples/lights.cc).

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
`light_problem` is public, for a caller holding a light from elsewhere.

## On the GPU

`sv::light_gpu` is one tagged 96-byte record per light, mirroring `sv::light` in shaders/light.hlsli.
Its `emission` is one canonical quantity per path: a point's intensity, a rect's radiance per face, a parallel light's irradiance, a sun's radiance.
A sun's radiance is its illuminance over `pi * sin(r)^2`, the projected solid angle of a disc of angular radius r, so a surface facing it receives exactly the lux it was given.
A cone is stored as the scale and offset of glTF's falloff, which an unshaped light turns into a constant 1, so no path branches on whether it has one.
The flatten groups them by path with `pt_light_table` — a counting sort over the walk it does anyway — and the frame block carries the count and each path's offset and length.
A loop over one path's run, like the bounce ray testing every area light, then runs in one branch.

Next-event estimation picks one light uniformly, and the `1/N` of that pick is inside `pt_light_pdf`.
That density is the one thing both estimators share, since the next-event sample and the bounce ray reaching a light weight against each other.
A density they disagree about makes the image wrong rather than noisy.
With exactly one light no pick is drawn, so a one-light scene keeps the sample sequence it always had.

A point or a parallel light is a delta that no sampled ray reaches, so it has next-event estimation alone and is divided by the pick probability only.
On a path's last surface hit no bounce ray follows, so next-event estimation takes the full weight there for every light and the sky;
weighting it against a ray that never flies would darken every rect and sun by the share left to that ray.
A rect and a sun have both strategies: the bounce ray credits every rect it crosses and every sun it escapes into, each weighted against its own density.

Lights occlude nothing — a shadow ray toward one passes straight through another — so the bounce ray credits every area light it crosses before the surface, not only the nearest.
Treating lights as transparent on one side and opaque on the other would leave part of the integral to neither strategy.

Two tests pin all of this.
`sv - every kind of light traces to the sum of each alone` traces one light of each path alone and all four together.
Light is additive, and dropping the pick from a density puts the sum off by a clear factor.
`sv - every kind of light delivers the illuminance its unit promises` puts each path above a floor at the same illuminance and asks the floor to agree.
That pins every unit conversion, and the sun's second strategy with it.

A layer with no lights of its own is traced under `layer::fallback_light`, a sun unless the layer sets another or turns it off.

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
   A non-uniformly scaled light takes the closest similarity, and glTF's `range` is stored and ignored, since it is a hint.
6. **Non-physical flags.** Landed.
   `visible_to_camera`, off by default and only for a light with an extent, and `casts_shadows`, on by default.
   A light that casts no shadow is ignored by geometry on both sides of the weighting, or the two would disagree about what they integrate.
   A light-linking mask is reserved in both GPU records, light and instance, all ones and read by nothing.

## Designed, not landed

- The `sphere` path, and a `disc` area shape.
- Tube and box emitters.
- Gobos and IES profiles, as further shaping kinds.
  When a two-dimensional texture sampler is built, it is built once for the environment map and shared.
- Power-weighted light selection, and the point at which emitters should move into the acceleration structure.
- A stable id per scene item, which picking will want; it is `stable_id<Tag>` again.

## Lower-library gaps

- **typed-geometry has no orthonormal-basis routine.**
  Building a light's frame from a direction, and a tangent frame from a normal, both hand-roll one (`frame_with_z` in light.cc, `any_perpendicular` in asset/impl/tangent_frames.cc).
- **typed-geometry types are not hashable.**
  A CPU hash of a light would have to fold `tg::vec3f`, angles and transforms by hand.
  Nothing needs one yet: the trace hash covers the uploaded bytes, which is where equal lights must hash equal.
