# SGL Ray tracing

SGL traces rays two ways, and a device grants each apart: an **inline trace** from any stage, and a **ray-tracing pipeline** of stages the GPU schedules.
This file is the model — the vocabulary both share, each half's declarations, and what each target writes.
[CHK-320 to CHK-332 and CHK-342 to CHK-345](semantics/checking.md#ray-tracing) are its rules, and [AST-150 and AST-153](syntax/ast.md#pipelines) its syntax.
[EMIT-135 to EMIT-139](semantics/emitting.md#ray-tracing) are what a target writes.
The vocabulary is SGL source in `prelude/raytracing.sgl`, which a reader may open like any other file.
Back to the [specification](_index.md).

| half | feature | what the shader writes | dx12, vulkan | metal | webgpu |
|---|---|---|---|---|---|
| inline trace | `ray_query` | `world.trace(r, any_hit)`, and the answer comes back as a value | `RayQuery` | `intersection_query` | emulated in SGL, over sg's acceleration pool |
| pipeline | `raytracing_pipeline` | stages, hit groups and a `@raytracing pipeline`, and `trace(world, r, set.ray, mut p)` | DXR library exports | a kernel and visible functions over sg's tables | refused (`target-lacks-feature`) |

**The two halves share their vocabulary**: one `ray`, one set of flags, one decision, and hits that read alike whichever half produced them.
So a closest hit of a pipeline and the result of an inline trace are both a `triangle_hit`, and code that shades one shades the other.

## What a trace runs against

**`acceleration_structure[.geometry]` is a binding member: the TLAS a trace runs against** (CHK-320).
Its argument says what the structure holds, and it is required, since it picks how a trace against it is written:

| argument | holds | an inline trace gives |
|---|---|---|
| `.triangles` | triangle BLASes alone | `triangle_hit` |
| `.procedural` | BLASes of boxes alone, whose primitives an intersection decides | `procedural_hit[A]` |
| `.mixed` | both kinds | `mixed_hit[A]` |

```sgl
require ray_query

binding scene:
    world: acceleration_structure[.triangles]
    hits: mut buffer[float4]
```

A member of one needs `ray_query`, or `raytracing_pipeline` in a file that grants that one: either half may trace against it.
It is a resource like a texture, never a value, and a function of the program takes none; the prelude's `trace` does (CHK-324).
HLSL writes `RaytracingAccelerationStructure`, MSL `instance_acceleration_structure`, and WGSL nothing at all (EMIT-135).

## The vocabulary

* **`ray`** is `origin: pos3`, `direction: vec3`, `t_min: float = 0.0` and `t_max: float = 3.0e38`, in world space.
* **`ray_flags`** is DXR's `RAY_FLAG_*` without the prefix, a builtin `@bitflags` enum (CHK-321).
  Flags combine with `|`, intersect with `&`, and `flags.has(f)` asks whether every flag of `f` is set.
  They are `none`, `force_opaque`, `force_non_opaque`, `accept_first_hit_and_end_search`, `skip_closest_hit`,
  `cull_back_facing`, `cull_front_facing`, `cull_opaque`, `cull_non_opaque`, `skip_triangles` and `skip_procedural`.
* **`hit_decision`** is what an any-hit decision says of a candidate: `accept`, `ignore`, or `accept_and_end_search`.
* **`triangle_candidate`** is a triangle the traversal could not decide alone, a non-opaque one.
  It holds `t`, `ray`, the instance's `instance_id` and `instance_index`, `geometry_index`, `primitive_index`, `barycentrics` and `is_front_face`.
* **`triangle_hit`** is the nearest triangle a trace accepted, with the candidate's fields and `is_hit`.
  **Where `is_hit` is false, `t` is the ray's `t_max` and nothing else is set.**
* **`procedural_box`** is a box the traversal entered, which an intersection is handed.
  It holds `t_current`, the nearest hit accepted so far; `ray`; `object_ray`, the ray in the instance's object space; and the ids.
* **`report[A]`** is what an intersection reports of its box: `t`, `attributes: A` and `is_hit`.
  `report(t = t, attributes = a)` is a hit, and `report.none()` is none.
* **`procedural_candidate[A]`** and **`procedural_hit[A]`** are a triangle's candidate and hit for a box's primitive, carrying the attributes its intersection reported instead of barycentrics.
  A procedural hit that is no hit has undefined attributes.
* **`mixed_hit[A]`** is either kind's hit in one flat struct, whose `kind: hit_kind` is `none`, `triangle` or `procedural`.
  Its `barycentrics` and `is_front_face` mean something for a triangle alone, and its `attributes` for a procedural primitive alone.
  `h.triangle()` and `h.procedural()` give the single-kind hit, which `is_hit` only where the kind is theirs.

**A hit's transforms are typed by what they move**, so a direction never picks up a translation:

| method | on | moves |
|---|---|---|
| `h.to_world(p: pos3) -> pos3`, `h.to_world(v: vec3) -> vec3` | every hit and `triangle_candidate`; `procedural_candidate` and `procedural_box` take the point alone | object space to world space |
| `h.normal_to_world(n: vec3) -> vec3` | every hit | by the inverse transpose, so a normal stays perpendicular to its surface |
| `h.to_object(p: pos3) -> pos3`, `h.to_object(v: vec3) -> vec3` | every hit | world space to object space |

The fields whose names start with `_` hold the rows of both matrices; they are the prelude's, and no program reads them.

## Inline traces

**`world.trace(r, …)` traces a ray and hands back the nearest hit it accepted**, as a value, in any stage.
What a traversal cannot decide alone it asks of **decisions**, which the call hands over as functions:

| structure | call | the decisions |
|---|---|---|
| `.triangles` | `world.trace(r, any_hit)` | `any_hit: (triangle_candidate) -> hit_decision` |
| `.procedural` | `world.trace(r, intersection, any_hit)` | `intersection: (procedural_box) -> report[A]`, `any_hit: (procedural_candidate[A]) -> hit_decision` |
| `.mixed` | `world.trace(r, any_hit, intersection, procedural_any_hit)` | the triangle's any hit, then the procedural two |

* **An any-hit decision may be left out**, and every candidate is then accepted: `world.trace(r)`, `world.trace(r, intersection)`.
  An intersection may not, since nothing else knows what a box holds.
* **A decision is a function's name or an arrow lambda** (CHK-318), and the trace calls it inside its loop.
  It is inlined there, never called through anything, so a decision costs what its body costs (CHK-319).
  A lambda sees the names around it, `c => c.t < limit`.
* **`flags` and `mask` are named-only**: `flags = ray_flags.force_opaque`, `mask = 0x01`.
  The mask is ANDed with each instance's; the default 255 meets every instance.
* **`is_occluded(world, r)`** asks whether anything lies on a ray of triangles, stopping at the first hit it accepts: the shadow ray.
* `A` is deduced from the intersection's result (CHK-340), so a call names no type.

```sgl
fun cutout(c: triangle_candidate) -> hit_decision:
    if c.primitive_index == 1 => return hit_decision.ignore
    return hit_decision.accept

@compute(8, 8) fun trace_cells(@thread_id id: int3){scene}:
    let h = scene.world.trace(cell_ray(id), cutout)
    if h.is_hit => scene.hits[id.x + id.y * 8] = float4(h.t, h.barycentrics.y, h.barycentrics.z, 0.0)
```

**A trace needs `ray_query` where the entry point reaches it**, not where a binding merely holds a structure (CHK-322).
So `require ray_query` stands in the file, in a binding the entry point lists, or in its body, and a vertex stage that reaches no trace runs on a device without it.
Nothing restricts the stage: a pixel stage traces, and so may a vertex stage, which no GPU test covers yet.

### Procedural geometry

**An intersection is handed one box and reports at most one hit in it.**
It works in the box's object space, where the box and its primitive stand, through `b.object_ray`.
A report before the ray's `t_min` or beyond the box's `t_current` is no hit, whatever `is_hit` says.

```sgl
struct sphere_attributes:
    normal: vec3

fun sphere_report(b: procedural_box) -> report[sphere_attributes]:
    let centre = float3(2.0 * (b.primitive_index as float) + 1.0, 1.0, 1.0)
    let o = float3(b.object_ray.origin.x, b.object_ray.origin.y, b.object_ray.origin.z)
    let d = float3(b.object_ray.direction.x, b.object_ray.direction.y, b.object_ray.direction.z)
    let oc = o - centre
    let half_b = dot(oc, d)
    let disc = half_b * half_b - dot(d, d) * (dot(oc, oc) - 1.0)
    if disc < 0.0 => return report.none()
    let t = (-half_b - sqrt(disc)) / dot(d, d)
    let p = o + d * t - centre
    return report(t = t, attributes = sphere_attributes(vec3(p.x, p.y, p.z)))
```

The intersection runs for every box the ray enters, and the any hit decides a report only where the box's geometry is non-opaque, after the instance's forced opacity and the ray's flags.
An opaque box's report is accepted as it is, as an opaque triangle is.

### A mixed structure

```sgl
@compute(8, 8) fun trace_mixed(@thread_id id: int3){mixed_scene}:
    let h = mixed_scene.world.trace(
        mixed_ray(id)
        any_hit = cutout
        intersection = sphere_report
        procedural_any_hit = cutout_sphere
    )
    if h.kind == hit_kind.procedural:
        let n = h.procedural().attributes.normal
        mixed_scene.hits[id.x] = float4(h.t, n.x, n.y, n.z)
```

A mixed hit is one flat struct rather than a choice of two, since SGL has neither an optional type nor an enum that carries values; the [incubator](incubator/raytracing-futures.md) holds both.

### Native and emulated

**A trace has two forms, and every target writes exactly one** ([EMIT-135](semantics/emitting.md#ray-tracing)).
The prelude's `trace` holds both through an internal `by_target(native, emulated)`, and the flat tree keeps both until the legalizer picks one.

* **The native form** steps through the target's own query, HLSL's `RayQuery` or MSL's `intersection_query`, with each decision inlined into the branch of its candidate.
* **The emulated form** is a software traversal written in SGL, in the prelude and `@internal` (CHK-323), over sg's acceleration pool.
  WGSL writes it, since WebGPU has no query, and the interpreter runs it too, so a `test` traces against a pool its driver binds (EVAL-95).
  Its pool layout is an internal contract between sg and the prelude, [raytracing-polyfill.md](../raytracing-polyfill.md).
* `t` and the barycentrics of the two forms agree within a tolerance, never bit for bit, and the emulated triangle test is not watertight yet.

## The pipeline

A ray-tracing pipeline is declared in four layers, each of which names the one before it:

1. **a ray set**, `rays name:`, the ray types and the payload each carries;
2. **the stages**, entry points that take a ray type's payload;
3. **hit groups**, `hit_group name for set:`, each one row of the table;
4. **the pipeline**, `@raytracing pipeline name:`, which names the set, the raygen, a miss per ray type and the rows.

What the host adds beside them — rows of its own, callables of its own — is the host's side, which slib types ([its cheat sheet](../../../shaped-shader-library/cheat-sheet.md)).

### Ray sets

**`rays name:` declares a ray set: one member per ray type, whose type is its payload** (AST-150).

```sgl
struct radiance:
    t: float
    is_hit: int

struct shadow:
    is_lit: bool

rays path_rays:
    surface: radiance
    occlusion: shadow
```

**A ray type's position in its set is its address.**
It is the trace's ray contribution and its miss index, and the set's size is the geometry multiplier, so a hit group's record for ray `r` is row `g`'s record `g * ray_count + r`.
Reordering a set changes every table built from it, which is why it is one declaration the pipeline, the hit groups and the host all name.

### Stages

**`@raygen @miss @closest_hit @any_hit @intersection @callable` make an entry point of a ray-tracing stage** (CHK-326), each of which needs `raytracing_pipeline`.
Its payload is a `mut` parameter of a struct, the caller's place, which the stage reads and writes (CHK-328).
What the GPU hands it besides is a parameter of a prelude type, read from the target where the entry point starts.

| stage | takes | returns | may trace a ray type | may call a callable |
|---|---|---|---|---|
| `@raygen` | stage inputs alone | nothing | yes | yes |
| `@miss` | `p: mut T`, and `r: ray` if it reads the ray | nothing | yes | yes |
| `@closest_hit` | `h: triangle_hit` or `h: procedural_hit[A]`, `p: mut T` | nothing | yes | yes |
| `@any_hit` | `c: triangle_candidate` or `c: procedural_candidate[A]`, `p: mut T` | `hit_decision` | no | no |
| `@intersection` | `b: procedural_box` alone: no payload reaches it | `report[A]`, `A` a struct of the program | no | no |
| `@callable` | `p: mut T`, the caller's parameter | nothing | no | yes |

* **Every stage may take `@launch_id` and `@launch_size`**, both `int3`: which ray of the launch it runs for, and the launch's size (CHK-327).
* **A trace or a callable called from a stage whose row says no is `stage-not-allowed`**, since the prelude's builtins carry `@stages` (CHK-193).
* An entry point is no generic function, so a stage names its `A`: `h: procedural_hit[sphere_attributes]`.

```sgl
@raygen fun primary(@launch_id id: int3){traced}:
    let mut p = radiance(0.0, 0)
    let r = ray(
        origin = pos3((id.x as float) + 0.25, (id.y as float) + 0.5, -1.0)
        direction = vec3(0.0, 0.0, 1.0)
    )
    trace(traced.world, r, path_rays.surface, mut p)
    traced.hits[id.x + id.y * 8] = float4(p.t, 0.0, 0.0, 0.0)

@miss fun sky(p: mut radiance):
    p.is_hit = 0

@closest_hit fun shade(h: triangle_hit, p: mut radiance){traced}:
    let mut s = shadow(false)
    let flags = ray_flags.accept_first_hit_and_end_search | ray_flags.skip_closest_hit
    let o = h.ray.origin + h.ray.direction * h.t
    let r = ray(origin = o, direction = vec3(4.0, 0.0, 2.0), t_min = 0.001, t_max = 2.0)
    trace(traced.world, r, path_rays.occlusion, mut s, flags = flags)
    p.t = h.t
    p.is_hit = 1

@any_hit fun cutout(c: triangle_candidate, p: mut radiance) -> hit_decision:
    if c.primitive_index == 1 => return hit_decision.ignore
    return hit_decision.accept
```

### Tracing a ray type

**`trace(world, r, set.ray, mut p)` traces through the pipeline's tables** (CHK-329).
The third argument names a ray type, which is what tells this `trace` from an inline one, and the payload is a place of exactly that ray type's payload, marked `mut`.
`flags` and `mask` follow by name.
The call writes through the payload: what the miss or the closest hit wrote is there when it returns.

### Hit groups

**`hit_group name for set:` is one row of the table: a record per ray type** (CHK-330).
A record is `(closest_hit = f, any_hit = g)`, either left out, and `()` is an empty record: every candidate is accepted, and no closest hit runs.
A ray type the group does not name has an empty record too.

```sgl
hit_group textured for path_rays:
    surface = (closest_hit = shade, any_hit = cutout)
    occlusion = (any_hit = shadow_cutout)

hit_group spheres for path_rays:
    geometry = .procedural
    intersection = sphere
    surface = (closest_hit = shade_sphere)
    occlusion = ()
```

* **Each record's shaders carry the payload of their ray type**, so a group cannot hand one ray's shader another's payload.
* **`geometry` is `.triangles`, the default, or `.procedural`**, and a procedural group names its `@intersection`, which the whole row shares.
* **A record's shaders take what the group's geometry hits**: a triangle group's a `triangle_hit` or a `triangle_candidate`.
  A procedural group's take a `procedural_hit[A]` or a `procedural_candidate[A]`, of the `A` its intersection reports.

### The pipeline declaration

**A `@raytracing pipeline` names its shaders in a block of settings**, and a short form is `invalid-pipeline` (CHK-331):

| setting | value |
|---|---|
| `rays` | the ray set, which every other line is read against |
| `raygen` | the `@raygen` entry point, which every pipeline has |
| `miss.<ray>` | the `@miss` of that ray type, which carries its payload; a ray type may have none |
| `hit_groups` | a group, or a round list of them, in table order; `.host` last, for the rows the host adds |
| `max_recursion_depth` | an `int` literal from 1 to 31, declared beside `.host` alone |

```sgl
@raytracing pipeline path:
    rays = path_rays
    raygen = primary
    miss.surface = sky
    miss.occlusion = open_sky
    hit_groups = (textured, spheres)
```

**What a pipeline could state wrongly it does not state**: its payload size, its attribute size and its depth are derived.
The payload size is the largest payload of its set, and the attribute size the largest its groups' intersections report, a triangle's 8 bytes of barycentrics at least.

* **One binding layout serves every shader of the pipeline**, so their binding lists agree by position, as a raster pipeline's stages do, and they list one `@inline` binding at most.
* **Each listed group is for the pipeline's set.**

### The depth is the trace graph's

**Each ray type's trace reaches the ray types its miss and its closest hits trace, and that graph must be acyclic** (CHK-332).
A trace that reaches a shader tracing it again is `recursive-trace`, since no depth bounds it.
The pipeline's depth is the longest chain the raygen starts, and at least 1.
In the example the raygen traces `surface`, whose closest hit traces `occlusion`: depth 2.

**A host's groups are compiled apart, so nothing sees what they trace.**
A pipeline listing `.host` therefore declares `max_recursion_depth`, the bound the host's shaders must keep, and its own shaders' derived depth must fit within it.
A pipeline without `.host` declares none, since its derived depth is exact.

```sgl
@raytracing pipeline open_path:
    rays = path_rays
    raygen = primary
    miss.surface = sky
    miss.occlusion = open_sky
    hit_groups = (.host)
    max_recursion_depth = 2
```

### Callables

**`callables name = (f, g, .host)` is a table of `@callable` shaders of one parameter type** (AST-153, CHK-343).
**`table[i](mut p)` calls the one at run-time index `i`**, handing over a place of that type (CHK-344).

```sgl
@callable fun doubled(v: mut operand):
    v.x = v.x * 2.0

@callable fun negated(v: mut operand):
    v.x = -v.x

callables ops = (doubled, negated, .host)

@raygen fun apply_ops(@launch_id id: int3){operands}:
    let mut v = operand((id.x as float) + 1.0)
    ops[id.x % 3](mut v)
    operands.values[id.x] = float4(v.x, 0.0, 0.0, 0.0)
```

* **A table is the module's, not a pipeline's**: every ray-tracing pipeline of the module holds every table, packed in declaration order.
  So a table's place in the callable section is a constant of the module, and a stage that calls one compiles once, whichever pipeline it joins.
* **`.host` stands last in its table, and a table taking it is the module's last**, since the host's callables follow every listed one.
  An index past a table's listed callables reaches what follows them, which is only the host's for that last table.
  Nothing checks an index at run time.

### What a payload states, on HLSL

DXR's payloads are `[raypayload]`, and every field states which shaders read and write it ([EMIT-137](semantics/emitting.md#ray-tracing)).
SGL infers that per field from what the module's shaders do: a stage reads and writes the fields its payload parameter does, and a caller the fields of the local it traces with.
Every shader of one pipeline must state the same qualifiers, and a host's group is compiled apart.
**So a payload type that a pipeline with `.host` groups traces, or that no pipeline of the module traces, states the widest access instead.**

## Per target

**dx12 and vulkan** write each stage as a DXR library export, `[shader("closesthit")]`, whose payload is `inout` (EMIT-136).
An any hit's `ignore` is `IgnoreHit()`, `accept_and_end_search` `AcceptHitAndEndSearch()`, and `accept` a plain return.
An intersection's report is `ReportHit`, and a procedural record's shaders take the attributes as a parameter of their own (EMIT-137).

**webgpu** traces inline through the emulated form, and has no pipeline: WebGPU has neither binding indexing nor function pointers, which a table needs.
A pipeline there waits for a polyfill, which the [incubator](incubator/raytracing-futures.md) sketches.

**metal** runs a pipeline as a kernel that intersects, then calls a miss or a closest hit through sg's tables (EMIT-139, CHK-345).

* The raygen is the kernel, and a miss, a closest hit and a callable are visible functions of one uniform signature, since one table holds every ray type's.
  Each takes the payload as `uint4` words, a hit record, and an `sgl_context` carrying the argument buffers, the tables and the launch.
* An any hit is a triangle intersection function.
* **A procedural group's record is one fused entry point, `sgl_<group>_<ray>`**: its intersection, the range check and the record's any hit.
* A record without a closest hit calls `sgl_empty_closest_hit`, since an empty table slot is no function.
* The closest hit's record is found from each instance's hit-group offset, which sg keeps in a buffer of its own at buffer 5.

Metal falls short of DXR in three places, each a [TODO](../TODO.md):

* **A hit's instance transforms are the identity** in a pipeline: Metal hands them only under intersection tags sg's tables do not declare.
* **`accept_and_end_search` acts as `accept`** in an intersection function.
* **One TLAS per dispatch**, whose instance offsets are the one buffer at buffer 5.

The metal pipeline is exercised by CI alone.

## Open

* A runtime-chosen ray type, several reports per intersection, and true recursion: the [incubator](incubator/raytracing-futures.md) holds them.