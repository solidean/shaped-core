# Ray-tracing futures

*Incubator: not normative.*

Where ray tracing goes after its first version, which [raytracing.md](../raytracing.md) specifies.

## The idea

The first version is DXR's model made static: a trace names its ray type, every function inlines, and every table offset is known when the module is checked.
Each idea below relaxes one of those, or reaches a platform the first version leaves out.

**True recursion.**
A pipeline refuses a trace graph with a cycle (CHK-332), where DXR allows recursion up to a declared depth.
A path tracer that bounces by recursion is the use, and a loop in the raygen is how SGL writes it today.
Allowing it would make `max_recursion_depth` a declaration the check pass cannot verify, so it wants a feature of its own, or a polyfill that unrolls a bounded recursion into the loop.

**A megakernel polyfill of the pipeline for WebGPU.**
WebGPU has no binding indexing and no function pointers, so it can hold no shader table, and the pipeline is `target-lacks-feature` there.
Every shader of a pipeline is known to the module but the host's, so one compute kernel could hold them all: the raygen, and a `switch` over a record index where DXR calls through the table.
The emulated inline trace is the traversal it would run.
The host's groups are what it cannot see, so a pipeline with `.host` would stay refused, or the host would hand its groups as SGL to compile into the kernel.
It is the same move [stage-polyfills.md](stage-polyfills.md) makes for the geometry and tessellation stages.

**Shader execution reordering.**
DXR 1.2's `MaybeReorderThread` and its hit objects let a closest hit run on a thread sorted by what it hit.
It would be a builtin behind a feature, and a hit object is a type the vocabulary would gain.

**Opacity micromaps and displacement micromaps.**
They are properties of a BLAS, which sg builds, so most of their cost lands in sg; SGL would gain little beyond a ray flag and a hit's micro-triangle fields.

**Motion blur.**
A ray carries a time, and a BLAS or an instance its motion; `ray` would gain a `time`, and the targets that have it would take it.

**Ray queries in the vertex stage.**
Nothing refuses an inline trace in a vertex stage, and the text every target writes looks right; no GPU test has run one yet.

**A ray type chosen at run time.**
`trace(world, r, set.ray, mut p)` names its ray type, so its payload type is known and the trace graph is static.
A run-time index would need every ray type of the set to share a payload, or a payload that is a union of them, and would make the trace graph every edge the index could take.

**Several reports per intersection.**
DXR lets an intersection call `ReportHit` several times, each decided by the any hit as it is made.
SGL's intersection returns one `report[A]`, which is the common case and what Metal's box intersection returns too.
Several would want a report stream, as the geometry stage's `emit` is.

**An optional `T?` and enums that carry values.**
A mixed trace gives one flat `mixed_hit[A]`, whose fields mean something for one kind at a time, and a report without a hit carries undefined attributes.
Both are the shape a language without sum types forces: `hit_kind` plus fields that are meaningful per kind, and `undefined()` where no value exists.
With `T?`, `report.none()` would be `none`; with payload-carrying enums, a mixed hit would be `case h: .triangle(t) => … .procedural(p) => …`.

**User-declared generic structs.**
The prelude declares `report[A]` and its neighbours, and a program may not declare a generic struct yet ([function-model.md](function-model.md)).

## What it touches

* The check pass: the trace graph (recursion), the stage table (reordering, micromaps, motion), and the type system (`T?`, enums with values).
* The emitters: a megakernel is a new shape of WGSL text, and a report stream a new shape of an intersection.
* sg: micromaps, motion and the megakernel's tables are sg's to build first.

## Already fixed by the syntax

* A ray type is a member access, `set.ray`, so a run-time choice could be an index into the set without new syntax.
* `@bitflags` enums exist for builtins, so a new flag is a case of `ray_flags`.

## Open

* Whether recursion is a feature of the device or a promise the shader makes with a declared bound.
* Whether a megakernel pipeline is emitted by SGL or assembled by slib from separately emitted functions.
* Whether `T?` and value-carrying enums are one mechanism, as in most languages that have both.
