# Quadric primitives

**Status: landed.**
**All seven phases below are in; what this document now describes is the code rather than the plan.**
**`examples/quadric-gallery.cc` is what the representation reaches; `examples/mesh-structure.cc` is it in practice, and `examples/mesh-structure-dense.cc` is it at mesh scale.**
**The [cheat sheet](../cheat-sheet.md) is the API.**

Analytic quadric surfaces as a second kind of scene item, traced by a custom DXR intersection shader over a procedural (AABB) BLAS.
sv draws exactly one kind of thing today — a triangle mesh, placed by a transform, shaded by a generated material permutation — and this is the second.

The motivating workload is technical rendering of a mesh's **structure** rather than its surface.
Vertices drawn as spheres and edges drawn as tubes, at mesh scale: tens of thousands to millions of primitives, re-authored every frame.
Tessellating those into triangles is what makes it expensive, and a quadric is a hundred bytes and one quadratic solve.

Most of what this needs already exists and none of it was built for this.
`sg` carries procedural raytracing end to end, in both the dx12 and vulkan backends.
`sg::blas_aabbs` and `build_blas` over AABBs are the geometry half, and `sg::hit_shader::intersection` picking the procedural hit-group type is the pipeline half.
Nothing in sv has ever used it.
`sv::scene_item_kind` was written with a second kind in mind and says so.
And `generate_material_shader` keys its compile on the runtime and epilogue includes as well as on the material.
So a second geometry kind is a second *spelling* of a material a caller already has, rather than a second material system.

## What a primitive is

A **quadric** is the zero set of a degree-2 polynomial, written as a symmetric 4x4 matrix Q over homogeneous points: a point x lies on the surface exactly when xᵀQx = 0.
Spheres, ellipsoids, cylinders, cones, paraboloids and plane pairs are all quadrics, which is why one representation and one intersection routine cover the whole set.
A ray substituted into one gives a quadratic, so there are at most two hit distances — the near root where the ray enters the surface and the far root where it leaves.

A quadric surface is infinite where a drawn shape is not, so a primitive is a surface **and** a region.

```cpp
/// The zero set of xᵀQx, as the 10 distinct entries of the symmetric 4x4 Q.
struct sv::quadric3
{
    tg::vec3f diag = {};     // xx, yy, zz
    tg::vec3f off_diag = {}; // xy, xz, yz
    tg::vec3f linear = {};   // xw, yw, zw
    float constant = 0.0f;   // ww
};

/// One primitive: a surface, and the region a hit has to lie in.
/// Both are expressed about `origin`, which is what keeps float32 honest at mesh scale.
struct sv::quadric_primitive          // 120 bytes on the CPU; 96 of them go to the GPU
{
    tg::pos3f origin = {};            // 12
    sv::quadric3 surface = {};        // 40
    sv::quadric3 clip = {};           // 40 — the solid is {surface <= 0} AND {clip <= 0}
    u32 flags = 0;                    //  4 — above all, whether the clipper's own surface is drawn
    tg::aabb3f bounds = {};           // 24 — the BLAS build input, in the SET's space; not part of the GPU record
};
```

**The clipper is a full quadric rather than a plane**, and that is what makes the representation closed under the shapes that matter.
A slab is itself a quadric — (x·n − d)² − h² is degree 2.
So a finite cylinder is a cylinder clipped by a slab, a cone frustum is a cone clipped by a slab, and a hemisphere is a sphere clipped by a plane pair.
All in the same 96 GPU bytes and the same shader.

### The origin is a correctness requirement, not a convenience

A general quadric stored in world space breaks at mesh scale, and it breaks silently.
A sphere of radius 0.008 centred at world x = 5000 has constant term |c|² − r², which is 2.5e7 − 6.4e-5.
The ulp of 2.5e7 in float32 is about 2, so the radius is gone before the shader runs.

Expressing the pair about a per-primitive origin and translating the ray into it is what avoids that.
**Pin it as a test**: a small-radius primitive far from the world origin, whose silhouette is checked rather than merely whose trace does not crash.

### A capsule is three primitives

The surface of a capsule is not degree 2 — it is a cylinder and two hemispheres, which is piecewise — so a round-capped edge is three records: a cylinder clipped to a slab, plus a sphere at each end.
That is 288 GPU bytes and three AABBs per edge, and it is the one place the choice of representation shows up in the primitive *count* rather than only in the bytes.

On the workload this exists for, the caps are redundant anyway.
A closed triangle mesh has roughly three edges per vertex.
So drawing vertices as spheres and edges as capsules is V spheres plus 3E = 9V more for the caps, and every one of those sits inside a vertex sphere that is already there.

```text
flat-capped cylinders + vertex spheres     E + V   ≈  4V primitives
capsules + vertex spheres                 3E + V   ≈ 10V primitives
```

So the mesh-structure path emits **flat-capped cylinders**, and the flat cap is exact there rather than an approximation: the joint is covered by a sphere the drawing already wanted.

A typed capsule tag would fit in the record as it stands and cost a branch and no stride change, so adding one later stays additive.
It is deliberately not in the first version.

## Batches, and the acceleration structures

**One procedural BLAS and one TLAS instance per batch.**
The batch's AABB buffer holds one box per primitive, and its primitive buffer holds one record per primitive.
The intersection shader reads `PrimitiveIndex()` into that buffer through the bindless table, exactly as a closest-hit today reads `InstanceID()` into the instance table.

The alternative, one TLAS instance per primitive over a shared unit BLAS, does not survive the target scale.
sv rebuilds its TLAS every frame, because refit is not implemented in sg yet.
So a million spheres would be a million `sg::tlas_instance` values built, hashed and uploaded per frame.
That is a cost proportional to the scene rather than to what changed, in a renderer whose whole resource model is built on unchanged things costing nothing.

**A batch is content-hashed and immutable**, exactly like `sv::triangle_geometry`.
That is what makes an unchanged batch upload and rebuild nothing.

The key is one XXH3-128 pass over the primitive span, taken lazily on the first `hash()` after a mutation.
Folding a digest per `add` was the first shape, and it is about five times the work for the same invariant.
The cost is the call count rather than the bytes, and `tests/quadric-set-benchmark.cc` is what settled it.
It is also what lands the streaming, residency and eviction story for free.
A quadric batch becomes the same kind of thing `mesh_manager` already holds: a pinned hashed payload that `ctx.stream` uploads, and that `record_pending_work` builds a BLAS behind.

**One material per batch**, so a multi-material set is several batches.
One BLAS can hold several geometries and DXR can select a hit group per geometry.
But that selection is multiplied by `MultiplierForGeometryContributionToHitGroupIndex`, which sv passes as **0** in both of its TraceRay calls.
Raising it would change hit-record indexing for every mesh in every trace, since the multiplier is a property of the ray rather than of the instance.
That is a repo-wide change to save a TLAS instance that costs 64 bytes.

## Authoring

`sv::quadric_set` is the real type, and it mirrors `sv::mesh`.
A value the caller owns and holds, with content-hashed payloads, a placement cache slot, an `is_ready`, a `material_id`, and parallel attribute arrays.

```cpp
auto set = sv::quadric_set();
set.name = "mesh structure";
set.material = steel;

for (auto const& v : m.vertices())
    set.add_sphere(tg::sphere3f(v.pos, 0.02f));
for (auto const& e : m.edges())
    set.add_line(tg::segment3f(e.from, e.to), 0.008f);

f.add_scene().add_quadrics(set);   // -> sv::quadric_ref
```

Immediate calls are sugar over the same type, for the case where holding a set is more ceremony than the drawing is worth:

```cpp
auto s = f.add_scene();
s.add_sphere(tg::sphere3f(p, 0.02f), steel);
s.add_line(tg::segment3f(a, b), 0.008f, steel);
```

They write into a frame-owned set, bucketed by material.
**That set hashes its contents, not its identity**, and the requirement is load-bearing rather than an optimization.
Without it the sugar is a trap: the same drawing that is free with an explicit set costs a full re-upload and BLAS rebuild every frame, and nothing in the API would say so.

The factories take tg objects — `tg::sphere3f` for a vertex, and `tg::segment3f` plus a radius or a style for an edge.
Per-primitive data is parallel attribute arrays on the set, the way `sv::mesh` carries `mesh_attribute`, rather than a value at the call site.
A heterogeneous push per primitive is the wrong shape at a million of them, and the parallel array is what the material system's frequency chain already knows how to read.

Placing a set hands back `sv::quadric_ref`, carrying `transform()`, mirroring `sv::mesh_ref`.

`add_line` takes an `sv::line_style` — a radius and how the ends are closed — and draws an OPEN tube when nothing says otherwise.
There is no polyline overload; a polyline is a loop at the call site.

## Materials

**A quadric permutation is a second spelling of the same generator.**
It is called with a quadric runtime include and a quadric epilogue include, in place of `material_runtime.hlsli` and `pt_material_hit.hlsli`.
`material_shader_key` already hashes those includes alongside the permutation key, so `material_shader_cache` holds both spellings of one material side by side with nothing new added to it.
The consequence is that a material placed on both a mesh and a quadric set compiles twice.
That is the price of the alternative being a divergent branch on geometry kind, inside the hottest shader in the renderer.

### Quadrics admit a SUBSET of the mesh frequencies

There is one frequency set, not one per geometry, and a geometry admits the part of it that its own primitives number.

```cpp
enum class sv::attribute_frequency : sv::u8
{
    per_instance, // both — one value for the whole placement
    per_vertex,   // mesh only
    per_corner,   // mesh only
    per_triangle, // both — one value per element of the primitive stream, indexed by PrimitiveIndex()
    per_edge,     // mesh only, and reserved
};
```

**That is what lets one material definition generate one shader body for both geometries.**
`per_triangle` means "one value per element of the geometry's own primitive stream" — a triangle for a mesh, a quadric for a
batch — so the generated load is the identical line of HLSL either way.
The name is the mesh's and the meaning is the index; renaming it was considered and dropped, because a geometry-neutral name
would read as a category containing `per_vertex` and `per_edge` rather than as a peer of them.

So the two geometries differ in the **preamble** alone: `make_context` builds a triangle's shading context from its corners and
barycentrics, `make_quadric_context` builds a quadric's from `PrimitiveIndex()`, and everything the material fragment reads is
the same afterwards.

A frequency the geometry cannot number loses to the next-coarsest rank, exactly as a format mismatch already did.
So an attribute list authored for a mesh is not fatal on a batch, and the other way round.

**A batch is one material.**
The underlying API takes a range of quadrics that share one, and a caller wanting per-quadric *parameters* gets them by being
bucketed into several batches — which is what `scene_ref::add_sphere` and `add_line` already do per material.
Anything finer than one value per primitive is therefore a question about materials rather than about frequencies.
A gradient along a tube, for instance, is a material that takes two parameter sets and interpolates between them; it is
deliberately not a frequency, and it is not built.

### The clipper has a surface of its own

The solid is **{surface ≤ 0} ∩ {clip ≤ 0}**, so its boundary has two parts.
Where `surface == 0` inside the clip region, the surface quadric is drawn.
Where `clip == 0` inside the surface region, the CLIPPER is — a cylinder's flat end caps, a hemisphere's floor — and one bit on
the primitive says whether that half is drawn at all.

A ray therefore meets up to **four** candidate points, two roots of each quadric, and each counts only where it lies inside the
other's interior.
The nearest survivor is the hit, and its normal is the gradient of whichever quadric it landed on.

That is what makes an open tube and a capped one **one record with one bit different**, rather than two pieces of geometry.

**The box bounds the solid and never the visible part of it.**
Toggling the emit bit changes which pixels are drawn and must not change the box by so much as a float: the box is the
acceleration structure's, and one that tracked visibility would make the same geometry two resources and a dropped hit a
silent hole.
A flat cap lies in the plane the clipper already cuts, so it adds nothing to the extent anyway — which is what makes the bit
free.


### A per-end frequency is the capability deliberately left out

An earlier revision of this design gave quadrics frequencies of their own — `per_quadric` for one value per primitive, and
`per_quadric_end` for a value at each end of a tube, blended along its length.
The clip slab makes the second nearly free: it is offset rather than centred, so it carries the axis WITH its sign, and the
blend parameter falls out of the same evaluation the intersection already does.

It was dropped, and the reason is the whole point of the section above.
A per-end frequency forks the generated body — one material would no longer produce one shader body for both geometries — and
that property is worth more today than a gradient along an edge.
`per_quadric` itself was redundant the moment `per_triangle` was read as "one value per element of the geometry's own primitive
stream", which is what it already meant.

So the gradient stays unbuilt rather than unconsidered.
The cheap way back to it is a frequency that reads the clip slab's axial parameter, and nothing in the record has to change for it.

### No textures on quadrics, for now

A general quadric has no natural surface parametrization, so there is nothing to sample by.
The texture ranks of the frequency chain are unreachable on a quadric set, and resolution says so rather than silently falling back to a coarser rank.

## What the intersection shader reports

```text
for each of the up to four roots — two of the surface quadric, two of the clipper:
    skip it unless t is in [RayTMin, RayTMax]
    skip it unless it lies inside the OTHER quadric
    keep it if it is nearer than the best so far
report the best, with the gradient of whichever quadric it landed on
```

The clipper's two roots are only considered when the emit bit is set, so an open tube solves one quadratic and a capped one two.
Still one `ReportHit` and no sorting.

**Taking the nearest survivor rather than the first root is load-bearing for ordinary geometry**, not only for interior views.
A slab-clipped cylinder is an open tube with nothing closing its ends.
Seen near end-on — which is what every edge pointing at the camera does — the near root lies on the cylinder outside the slab, and the clipper rejects it.
The far root is the inside of the opposite wall, and is genuinely visible.
Dropping the hit there would make an open cylinder disappear at exactly the view where it is most common.

The same second test covers a ray whose origin is inside the primitive, whose near root is behind `RayTMin`.

**AABBs are computed on the CPU**, by the factory that turns a tg object into a record, and stored in the batch's AABB buffer.
Nothing recomputes one on the GPU and nothing should: an intersection shader cannot read the acceleration structure's own boxes, so a box the shader needed would have to be duplicated into the record.

**Quadrics are opaque.**
`sg::tlas_instance::opaque_override` is forced true, so the hardware skips any-hit entirely and a cutout material cannot be used on a quadric set.
A transmissive material is untested rather than refused — the root rule above already supports it, so forbidding it would be a deliberate narrowing rather than a missing capability.

## What deliberately does not go into typed-geometry

Nothing here lands in tg.

There is no `tg::capsule` and no `tg::cylinder`; `add` takes a `tg::segment3f` and a separate radius.
There is no `tg::quadric`; sv carries `sv::quadric3` as its own type.
tg's own TODO wants a `quadric` for a different reason: it is what a `sphere` or `ellipsoid` becomes under a projective map, which is about closing the transform registration table.
That entry stands on its own schedule rather than being absorbed into a renderer feature.

There is no ray-primitive query either.
The math has to exist in HLSL because that is where it runs.
And tg's query layer is explicitly gated behind its representations settling — see [typed-geometry's structure](../../../base/typed-geometry/docs/structure.md).
A CPU reference implementation for testing lives beside sv's test.

## The raster path

Not designed here, and no decision above was made in anticipation of one.

A quadric rasterizes as a coarse proxy with the same quadratic solved per pixel and the true depth written, and that works identically whatever the record holds.
The one thing recorded as intent is that **the primitive buffer stays a plain bindless byte-address buffer whose indexing assumes no ray-tracing stage**, which the batching decision already produces.
Depth semantics — conservative depth costs the early-z rejection that is the raster path's main saving — are not decidable before a raster path exists.

## Phasing

Each step is meant to be landable and testable on its own.

1. **`sv::quadric3` and `sv::quadric_primitive`** — landed.
   The CPU factories, their AABBs, and `sv::intersect` as the reference the shader is written against.
2. **`sv::quadric_set` and its content hash** — landed, with `sv::resident_quadric_set` as the id-only form.
3. **`quadric_manager`** — landed, beside `mesh_manager` and draining through `gpu_resource_manager`.
4. **The intersection shader and the quadric epilogue** — landed.
   `quadric_runtime.hlsli` adds the quadric decode and solve to `material_runtime.hlsli` rather than forking it, and
   `pt_quadric_hit.hlsli` is the epilogue.
   The shading tail both geometry kinds share moved into `pt_shade.hlsli`, so a hit is located per kind and shaded once.
   `material_permutation` gained an `intersection` shader, and `pathtrace_routine` puts it on BOTH of a permutation's
   records — the shadow one too, since a shadow ray traverses the same procedural BLAS.
5. **The material fork** — landed.
   There is ONE frequency set and no quadric-only frequency: resolution runs against a `geometry_view` that says which kind the
   geometry is, `sv::serves` says which frequencies that kind can number, and one it cannot loses to the coarser rank like any
   other unusable candidate.
   A batch therefore admits `per_instance` and `per_triangle` alone, which is what lets one material definition generate one
   shader body for both geometries — the two differ in the preamble that builds the shading context and in nothing the material
   fragment reads.
6. **The authoring surface** — landed.
   `scene_ref::add_quadrics` over either form, `sv::quadric_ref`, and the immediate `add_sphere` / `add_line` / `add_arrow`
   sugar over a frame-owned batch per (view, layer, material).
   The sugar's batch is flushed once, before the frame is flattened, because a batch is ONE scene item and is not placeable
   until it is complete.
   `scene_item` gained a `quadric_set` arm and `view_renderer` builds its TLAS instance; a batch still streaming is drawn as
   the shared placeholder cube through the TRIANGLE fallback, since a procedural hit group on a triangle BLAS is exactly the
   mismatch that refuses to build.
7. **Examples** — landed, four of them, each with a committed capture.
   `quadric-gallery.cc` is the showcase: a sphere, an ellipsoid, an open tube, a capped one, a capsule, a hemisphere, a cone
   frustum and a hyperboloid, side by side — and the same batch placed a second time under a non-uniform scale, which turns
   its spheres into ellipsoids for free.
   The open and capped tubes are the same record with one bit different, which is the clearest thing in the picture.
   `quadric-arrows.cc` is the arrow API: an axis frame, and the same eight segments drawn twice — proportional in one row,
   at a fixed shaft radius in the other — which is the difference the sizing overload makes and the reason there are two.
   `mesh-structure.cc` is the feature taught small: an icosahedron's 42 primitives, coloured at `per_triangle` — one value per
   quadric, each vertex taking its own colour and each edge the average of its two endpoints'.
   `mesh-structure-dense.cc` is the same authoring code at the scale a real mesh has — a five-times-subdivided
   icosahedron, 10,242 vertices and 30,720 edges as **40,962 primitives in one batch**, one acceleration structure, one
   instance.
   Its edges are coloured by their own length, again at `per_triangle`, which draws the construction's seams as a pattern
   rather than a number.

## Elsewhere

- [structure](structure.md) — where this sits in the module roadmap.
- [asset-loading](asset-loading.md) — the mesh importer, and the CPU / GPU type split this design mirrors.
- [graphics](../../../../docs/graphics.md) — the graphics family, and where `sg`'s procedural raytracing lives.
