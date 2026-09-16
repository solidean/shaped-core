# Quadric primitives (plan)

**Status: phases 1 to 4 have landed — the record, the batch, the manager, and the first trace.**
**Phases 5 to 7 are still design: the material fork, the authoring surface, and an example.**

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
struct sv::quadric_primitive          // 92 bytes
{
    tg::pos3f origin = {};            // 12
    sv::quadric3 surface = {};        // 40
    sv::quadric3 clip = {};           // 40 — a hit is kept where xᵀCx <= 0
};
```

**The clipper is a full quadric rather than a plane**, and that is what makes the representation closed under the shapes that matter.
A slab is itself a quadric — (x·n − d)² − h² is degree 2.
So a finite cylinder is a cylinder clipped by a slab, a cone frustum is a cone clipped by a slab, and a hemisphere is a sphere clipped by a plane pair.
All in the same 92 bytes and the same shader.

### The origin is a correctness requirement, not a convenience

A general quadric stored in world space breaks at mesh scale, and it breaks silently.
A sphere of radius 0.008 centred at world x = 5000 has constant term |c|² − r², which is 2.5e7 − 6.4e-5.
The ulp of 2.5e7 in float32 is about 2, so the radius is gone before the shader runs.

Expressing the pair about a per-primitive origin and translating the ray into it is what avoids that.
**Pin it as a test**: a small-radius primitive far from the world origin, whose silhouette is checked rather than merely whose trace does not crash.

### A capsule is three primitives

The surface of a capsule is not degree 2 — it is a cylinder and two hemispheres, which is piecewise — so a round-capped edge is three records: a cylinder clipped to a slab, plus a sphere at each end.
That is 276 bytes and three AABBs per edge, and it is the one place the choice of representation shows up in the primitive *count* rather than only in the bytes.

On the workload this exists for, the caps are redundant anyway.
A closed triangle mesh has roughly three edges per vertex.
So drawing vertices as spheres and edges as capsules is V spheres plus 3E = 9V more for the caps, and every one of those sits inside a vertex sphere that is already there.

```text
flat-capped cylinders + vertex spheres     E + V   ≈  4V primitives
capsules + vertex spheres                 3E + V   ≈ 10V primitives
```

So the mesh-structure path emits **flat-capped cylinders**, and the flat cap is exact there rather than an approximation: the joint is covered by a sphere the drawing already wanted.

A typed capsule tag would fit in 32 of the 92 bytes and cost a branch and no stride change, so adding one later stays additive.
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
    set.add(tg::sphere3f(v.pos, 0.02f));
for (auto const& e : m.edges())
    set.add(tg::segment3f(e.from, e.to), 0.008f);

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

`add` takes tg objects — `tg::sphere3f` for a vertex, and `tg::segment3f` plus a separate radius for an edge.
Per-primitive data is parallel attribute arrays on the set, the way `sv::mesh` carries `mesh_attribute`, rather than a value at the call site.
A heterogeneous push per primitive is the wrong shape at a million of them, and the parallel array is what the material system's frequency chain already knows how to read.

Placing a set hands back `sv::quadric_ref`, carrying `transform()`, mirroring `sv::mesh_ref`.

`add_line` draws a capsule by default, with a flag for flat caps.
There is no polyline overload; a polyline is a loop at the call site.

## Materials

**A quadric permutation is a second spelling of the same generator.**
It is called with a quadric runtime include and a quadric epilogue include, in place of `material_runtime.hlsli` and `pt_material_hit.hlsli`.
`material_shader_key` already hashes those includes alongside the permutation key, so `material_shader_cache` holds both spellings of one material side by side with nothing new added to it.
The consequence is that a material placed on both a mesh and a quadric set compiles twice.
That is the price of the alternative being a divergent branch on geometry kind, inside the hottest shader in the renderer.

### Quadrics carry their own geometric frequencies

`per_vertex` and `per_corner` have no reading on a quadric, so quadrics get their own values in the same enum and resolution rejects a frequency the geometry cannot serve.

```cpp
enum class sv::attribute_frequency : sv::u8
{
    per_instance,   // one value for the whole placement      — both geometries
    per_vertex,     // mesh only
    per_corner,     // mesh only
    per_triangle,   // mesh only    — indexed by PrimitiveIndex()
    per_edge,       // mesh only

    per_quadric,     // quadric only — indexed by PrimitiveIndex()
    per_quadric_end, // quadric only — two values, blended by the clip parameter
};
```

`per_triangle` keeps its name.
It is a mesh-only name that says exactly what it indexes, and `per_quadric` sits beside it doing the same job for the other geometry — so neither reads as a category containing `per_vertex`.

Only the geometric frequency forks.
`material_frequency`, the rank chain from type default down to texture, is untouched.

### The clip slab is the interpolation axis

`per_quadric_end` blends two values along the primitive, and the parameter is free: the clipper for a finite cylinder is a slab, and a slab **is** an axis, an offset and a half-length.

```text
t = saturate( ( dot(hit, n) - d + h ) / max(2h, eps) )
```

Every term is data the intersection shader has already loaded and used to accept the hit, so there is nothing extra in the record and nothing for a caller to supply.
A primitive with no meaningful clip has h = 0 and the guarded divide pins it to t = 0, which is the right answer: a sphere has no two ends to blend between.

An edge that fades along its length, or a cone whose colour tracks its taper, is the technical-drawing vocabulary this feature exists to serve, and it costs one lerp.

### No textures on quadrics, for now

A general quadric has no natural surface parametrization, so there is nothing to sample by.
The texture ranks of the frequency chain are unreachable on a quadric set, and resolution says so rather than silently falling back to a coarser rank.

## What the intersection shader reports

```text
accept the near root if  t >= RayTMin  and  the clipper admits it
else accept the far root if  t >= RayTMin  and  the clipper admits it
else report no hit
```

One `ReportHit`, no sorting, and one extra compare over reporting the near root unconditionally.

**The far-root fallback is load-bearing for ordinary geometry**, not only for interior views.
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
5. **The material fork**: the quadric runtime include, the two new frequencies, the clip parameter, and resolution rejecting what a quadric cannot serve.
6. **The authoring surface**: `add_quadrics`, `sv::quadric_ref`, and the immediate `add_sphere` / `add_line` sugar.
7. **An example** drawing a loaded mesh's vertices and edges, with a committed capture.

## Elsewhere

- [structure](structure.md) — where this sits in the module roadmap.
- [asset-loading](asset-loading.md) — the mesh importer, and the CPU / GPU type split this design mirrors.
- [graphics](../../../../docs/graphics.md) — the graphics family, and where `sg`'s procedural raytracing lives.
