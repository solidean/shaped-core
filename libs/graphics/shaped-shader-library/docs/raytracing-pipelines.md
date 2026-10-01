# Ray-tracing pipelines from SGL

An SGL `@raytracing pipeline` declaration generates a C++ type that builds the `sg::raytracing_pipeline_description` and the shader table for it.
**The host writes no shader handle and no table index by hand**: every one is a constant of the order below, and the generated type hands out the rest.
What the declaration leaves `.host` — hit groups, callables — the host compiles from SGL at run time and hands over.

sg's [raytracing-pipeline](../../shaped-graphics/docs/concepts/raytracing-pipeline.md) is the pipeline and table this fills in, rows and all.
The declaration itself is SGL's, in its [ray-tracing spec](../../shaped-graphics-language/docs/spec/raytracing.md).
[raytracing_pipeline.hh](../src/shaped-shader-library/raytracing_pipeline.hh) is the API the generated type calls.

## One pipeline, end to end

A package declares the file with `path:*`, or the one pipeline as `path:raytracing_pipeline:<name>`.
The pipeline's binding lists must be generated types too, so `path:*` is the usual spelling.

```text
rays path_rays:
    surface: radiance
    occlusion: shadow

hit_group textured for path_rays:
    surface = (closest_hit = shade, any_hit = cutout)
    occlusion = (any_hit = shadow_cutout)

@raytracing pipeline path:
    rays = path_rays
    raygen = primary
    miss.surface = sky
    miss.occlusion = open_sky
    hit_groups = (textured, spheres)
```

```cpp
using path_t = shaders::raytracing_pipeline_path_t;          // <file stem>_<pipeline>_t

auto const desc = co_await shaders::raytracing_pipeline.path.description(ctx);
auto const pipeline = co_await ctx.cached.acquire_raytracing_pipeline(desc);

auto table_desc = path_t::table_description(pipeline);        // raygen, a miss per ray type, every callable
auto const row = path_t::add_row(table_desc, path_t::hit_groups_t::textured);
auto const table = ctx.uncached.create_raytracing_shader_table(table_desc);

auto const blas = cmd->raytracing.build_blas(triangles, sg::accel_build_flag::fast_trace, path_t::ray_count);
sg::tlas_instance const instances[] = {{.blas = blas, .instance_id = 10, .hit_group_offset = table->offset_of(row)}};
auto const tlas = cmd->raytracing.build_tlas(instances);
cmd->raytracing.bind_pipeline(*pipeline);
cmd->raytracing.bind_group(0, *group);                        // a generated binding holding .world = tlas->as_view()
cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), width, height);
```

`libs/graphics/shaped-graphics/tests/pipeline/raytracing-pipeline-test.cc` runs this shape on every backend with the pipeline, two ray types and a procedural group included.

## The generated type

| member | what it is |
|---|---|
| `ray_count` | the ray types of the pipeline's ray set; a BLAS it traces is built with it as `hit_record_stride` |
| `hit_group` | a hit group's position among the table's groups, `{int index}`, a type of this pipeline's own |
| `hit_groups_t::<name>` | a listed hit group's `hit_group`, for `add_row` |
| `first_host_hit_group` | the `hit_group` the host's first one takes, when the declaration lists `.host`; the next is `{first_host_hit_group.index + 1}` |
| `first_host_callable` | the index a shader calls the host's first callable by, when its callables list `.host` |
| `description(ctx, host = {})` | the `sg::raytracing_pipeline_description`, its shaders compiled for `ctx`; async and cold |
| `table_description(pipeline, host = {})` | a table with the raygen, a miss per ray type and every callable, the host's in `host` included; rows follow |
| `add_row(table, group)` | appends group `group`'s row, one record per ray type, and returns the `sg::hit_row` |
| `definition()` | the `slib::raytracing_pipeline_definition` all of the above is computed from |

An instance of the type is the file symbol's member, `shaders::<stem>.<pipeline>`; the rest are static.
**A group index is typed per pipeline**, so another pipeline's group, or a bare `int`, does not compile as an argument of `add_row`.
**`hit_record_stride` is the caller's to pass**, since the BLAS is built before any table exists: metal bakes it into the structure, and a wrong one reaches the wrong record there.
`dispatch_rays` logs that mismatch under `ctx.portability_checks()`.

## Everything is registered in one fixed order

That order is what makes each index a compile-time constant rather than a lookup.

- **raygen** handle 0 is the pipeline's raygen.
- **miss** handle r is ray type r's miss, in the ray set's order, so a trace of ray type r misses into r.
- **hit** handle `g * ray_count + r` is hit group g's shader for ray type r: the listed groups first, then the host's in the order handed over.
- **callable** handle k is the module's callables in their declared order, then the host's.

A table places its raygen, misses and callables at the index equal to the handle, and `add_row(table, g)` places group g's `ray_count` hit shaders consecutively.
So a row's `offset_of` is what an instance takes, and ray type r of geometry i reaches `offset + i * ray_count + r` on every backend.

## `.host` hit groups and callables, compiled at run time

A declaration may leave its hit groups open, `hit_groups = (.host)`, and a callables list may end in `.host`.
The host then supplies them through `slib::raytracing_host_parts {hit_groups, callables}`, which `description` takes.

```cpp
using open_t = shaders::raytracing_open_open_path_t;
auto const hits = co_await slib::compile_hit_group(ctx.get(), &library, &open_t::definition(), source, "material", "runtime.sgl");
auto const desc = co_await shaders::raytracing_open.open_path.description(*ctx, {.hit_groups = hits});
// …
auto const row = open_t::add_row(table_desc, open_t::first_host_hit_group);
```

- **`compile_hit_group` checks the group against the pipeline by the ray set, whole.**
  The set's name, every ray type's name in order, and each payload's struct name, byte size and shape must all be the pipeline's.
  So a restated payload with a field more, or two ray types swapped, is refused rather than traced with the wrong size.
- A group for another set, a group the source lacks, a source that does not compile, or a pipeline without `.host` groups is an async error, never a throw at the call.
- It returns `ray_count` hit shaders, which is the unit `raytracing_host_parts::hit_groups` counts in.
- **`compile_callable`** compiles one `@callable` entry point, and takes the pipeline's definition too.
  The callable must take the parameter of the pipeline's `.host` callables, by name and by shape.
- **The table takes the same `host` the description did**, `table_description(pipeline, host)`, so the host's callables get records too.
  A pipeline whose callables are all the module's asserts on a `host` holding one.
- Both compile SGL into the first format the context accepts that `library` has a compiler for, as an asset's `acquire` would pick.
- A part the declaration leaves closed must stay empty in `host`, which `description` asserts.

A runtime compile reads the library's module directories like any other, so the host's source may `use` a module; nothing exercises a ray set declared in one yet, and the host's source restates the ray set and the payload structs today.

## Hot reload

A reload of the pipeline's source is followed where it changes a shader's body, as any shader's is.
**What the generated type fixes is frozen**, since the host's code and tables were built against it.
That is the ray set with each payload's shape and size, the raygen, the misses, each hit group and its records, and the callables.
It is also the recursion depth, the payload and attribute sizes, the layout, the samplers and the features.
A reload that moves any of these keeps the module's shaders the pipeline was last described with on that context, and the log says what moved.
`slib::frozen_moved_of(definition)` answers the same question for a host that asks it itself.
A source whose frozen part moved before the pipeline was ever described on a context has nothing to keep, and fails.

## What metal needs, and slib supplies

Metal runs a ray-tracing pipeline as one kernel per raygen.
sg's [metal readme](../../shaped-graphics/backends/metal/readme.md#ray-tracing-both-paths-and-a-raygen-shader-that-is-the-kernel) has the whole mapping.
Two of its constraints reach this library, and `description` and `compile_hit_group` both meet them when the context takes MSL or a metallib.

- **A procedural record runs one fused traversal function.**
  Metal runs exactly one function during traversal, and sg refuses a procedural group that carries an intersection and an any-hit.
  SGL writes a traversal per record that does both, and slib registers it as the group's intersection with no any-hit.
  The group's plain intersection is never compiled for metal.
- **Every closest-hit slot holds a function.**
  SGL's kernels call the closest hit of whatever record they reach, and an empty metal table slot is no function.
  So a record without a closest hit gets `sgl_empty_closest_hit`, which does nothing.

Neither changes the order above or what the host writes.
On the other backends the description is the plain DXR shape.

## Where the generator reads from

`cmake/sgl_host_code.py` writes the type from `sgl describe`'s JSON: `ray_sets`, `hit_groups` with each record's `traversals`, and `raytracing_pipelines`, each with its own `callables`.
A pipeline's file samplers reach its layout as a raster pipeline's do, each at its index among the file's samplers.
Two declarations are refused at generation rather than at run time.

- **A pipeline whose binding list names a group that was not generated**; declare the file as `path:*`.
- **A ray type without a miss**, which sg's table has no empty-record spelling for yet.

webgpu has no ray-tracing pipeline, so a host gates on `ctx.supports(sg::feature::raytracing_pipeline)` before it describes one.
