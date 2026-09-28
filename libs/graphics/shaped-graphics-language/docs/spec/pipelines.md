# SGL Pipelines

A **pipeline** is what a GPU draws or dispatches with: shader stages compiled together with the configuration baked into them.
This file is the model — what a `pipeline` declaration says, how its settings are read, and what it checks.
[AST-131 to AST-134](syntax/ast.md#pipelines) is the syntax.
Back to the [specification](_index.md).

```sgl
@vertex fun main_vs(v: cube_vertex){constants} -> pixel_input:
    return {position = constants.view_projection * v.position, normal = v.normal, color = v.color}

@pixel fun main_ps(p: pixel_input) -> target:
    return {color = float4(..p.color, 1.0)}

pipeline:
    vertex = main_vs
    pixel = main_ps
    cull = .back
    depth_test = true
    depth_write = true
    depth_stencil_format = .depth32_float
    format = .host
```

The host then acquires it by name, and states nothing the shader already said: not the vertex layout, not the binding layout, not the targets.

## A pipeline and its stages

* **A declaration names its entry points by stage**: `vertex = main_vs`, `pixel = main_ps`.
  The stage slots are named after SGL's stages ([terminology](terminology.md)), since what fills one is an SGL entry point.
* **The short form lists them instead**: `pipeline shadow = (shadow_vs, shadow_ps)`.
  Each entry point goes to the slot its stage attribute names, so their order is free, and two of one stage are an error.
  Settings go in a block under the list, `pipeline shadow = (shadow_vs, shadow_ps):`, the same ones a declaration takes.
* **A pipeline without a name is named `pipeline`**, which is the one a file with a single pipeline usually has.
  A pipeline shares its file's names with the file's entry points, so it cannot be called like one.
* **The kind is an attribute**: `@raster`, which is the default, `@compute` and `@raytracing`.
  A compute pipeline needs no declaration, since one shader and its binding list are the whole of it: every compute entry point is its own.

## Settings are assignments

A pipeline's configuration starts at `sg`'s defaults, and each setting assigns one part of it, top to bottom.
A later line overrides an earlier one.

* **The left side is a path into `sg::raster_pipeline_description`**, with `sg`'s names and nesting: `rasterization.cull`, `depth_stencil.depth_compare`.
  The prelude mirrors the description as ordinary SGL types, so a path is checked the way a field access is.
* **What the entry points state is not a setting**: the binding layout, the vertex input and the target set follow from them, and naming one is an error.
* **`.name` on the right is a case of the setting's enum**, the way it is a case of a `case` scrutinee.
* **A paren literal on the right replaces the whole part**, and names every field of it.
  `depth_stencil = (depth_test = true)` is an error for the fields it leaves out; a single field is set by its own path.
* **`blend = .none`** switches blending off, which is the one setting an optional part has beside its fields.
* **A write mask is four flags**, `write_mask = (r = true, g = true, b = true, a = false)`, since SGL has no set of enum cases.

### A name stands for its path while it is unique

Writing every path in full would make `rasterization.cull` the common spelling, so a setting may leave the path out.

* **A name that is no field of the description is looked for below it**, and stands for the one field of that name.
* **A name found twice is an error that names both paths**, and the setting is then written in full.
* **A setting under `color_targets` means every target**: `format = .rgba8_unorm` is each target's format.
* **One target is set by its full path**, `color_targets.albedo.blend`, where `albedo` is a member of the pixel stage's `@pixel struct`.
  A target's name never stands for its path, so renaming a target cannot make another line ambiguous.

```sgl sketch
pipeline gbuffer:
    vertex = scene_vs
    pixel = gbuffer_ps
    cull = .back                               // rasterization.cull
    format = .rgba8_unorm                      // every target
    color_targets.normal.format = .rgba16_float // then one target otherwise
```

### Attributes on the code are settings too

A shader that is only correct under one configuration says so where it is written.

* **`@name(value)` on an entry point or an edge struct is the setting `name = value`**, found the same way a setting's name is.
* **On a member of a `@pixel struct` it is that target's**: `@format(.rgba16_float) normal: float4`.
  `@format` is one of these, which is how a shader pins a target's format.
* **Stage names are not settings**, since `@vertex` and `@pixel` already mark a stage.

The sources assign in this order, each over the last:

1. `sg`'s defaults;
2. the attributes of the edge structs: the vertex input, then the `@pixel struct`;
3. the attributes of the stages, in pipeline order;
4. the declaration's settings, top to bottom.

Two sources of one step that set one field differently are an error, unless the declaration sets that field itself.

## What a pipeline checks

A pipeline is the first place two stages meet, so it is where they are checked against each other.

* **Adjacent stages pass one interface**: what the vertex stage returns has the members the pixel stage takes, with the same names and types, in the same order.
  A location is a member's position ([EMIT-26](semantics/emitting.md#addresses)), and each stage is compiled apart ([EMIT-6](semantics/emitting.md#targets)).
  So agreeing member for member is what makes the slots agree.
* **The binding lists agree by position**: with `@inline` left out, each stage's list names the same binding as the longest one at every position it has.
  The pipeline's binding layout is that longest list, and all its stages list one `@inline` binding at most.
* **Every target has a format**: stated by a setting, by `@format`, or left to the host with `.host`.

## Formats the host states

A few parts of a pipeline are only known when the program runs, such as the format of the window a frame is presented to.

* **`.host` leaves a format or the sample count to the host**: `@format(.host) color: float4`, `sample_count = .host`.
* The host then states it when it acquires the pipeline, and a pipeline with nothing left open is acquired with nothing.

## What the host sees

Everything a pipeline states reaches the host as one generated symbol per pipeline, which `ctx.cached` acquires; slib's [cheat sheet](../../../shaped-shader-library/cheat-sheet.md) has its spelling.

* **The frozen part is what the host's own code was built against**: the binding layout, the vertex input, the target set, the features its stages need of a device, every format, and the sample count.
  The features are frozen because the host chose its device by them: a reload that needs one more could be refused by a device the build ran on.
  A struct or binding in it is compared by its name and its shape, the structural hash of its members, so a member added under the same name is a change.
  It never changes under a hot reload.
* **The rest reloads**: topology, rasterization, the depth and stencil tests, blending, write masks.
* A reload that changes the frozen part keeps the stages and settings it last built with, and says what changed.

## What the compiler carries today

[CHK-174 to CHK-187](semantics/checking.md#pipelines) is what the check pass carries: the stages, the settings with their names and their fan-out, the attributes, and the checks between the stages.

* A raster pipeline of a vertex and a pixel stage, or of a vertex stage alone.
* A value is a literal, `true`, `false`, a case, `.host`, `.none`, or a paren literal of those.

## Open

* The stages beyond vertex and pixel, which have no stage attribute yet; a pipeline checks each adjacent pair of them the same way.
* Named blends in the prelude, `blend = .premultiplied_alpha`, which wait on `const` or static members.
* The settings of a `@raytracing` pipeline.
