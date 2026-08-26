# shaped-viewer cheat sheet

Professional, RTX-enabled visualization renderer.
Namespace `sv`.
Depends on shaped-rendering.
Headers are included by full path from `src/`: `#include <shaped-viewer/<name>.hh>`.
The authoring surface sits at the root; everything else is one folder down — `scene/` (what goes into a view), `view/` (what a view is), `layout/`, `resources/`, `rendering/`.

> **Scope note:** early stage, in three layers.
> A frame is authored through the fluent handles (`sv::interactive` → `frame` → `window_ref` → `view_ref` → `scene_ref`), flattened into a `render_plan`, and replayed by `viewer_renderer`.
> A **view is the definition of one texture**, and a view's layer may itself be a whole layout tree — so views nest, at any depth.
> Rendering needs a ray-tracing backend: dx12 + DXR on Windows today, since vulkan RT is stubbed upstream.
> The API is present everywhere; without a backend a routine just draws nothing.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-viewer/all.hh>   // umbrella
```

**Recording domain:** `sv`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

## Per-frame description — what to render

```cpp
sv::viewer_definition            // { vector<view_data> views; layout_tree nodes; view_index root_view; def[i] -> view_data& } — a whole frame, as two flat pools
sv::view_data                    // { view_id id; vec2i resolution; bool resolution_follows_layout; camera; vector<layer> layers; refresh_policy refresh; vector<temporal_input>; }
                                 //   the definition of ONE TEXTURE. Deliberately no position: the leaf referencing it decides where it goes,
                                 //   which is what makes "relayout must not restart a converged image" a property of the type
sv::layer                        // { layer_kind kind; layer_blend blend; float opacity; layout_node_id root_node; vector<scene_item> items; vector<area_light>; background; render_settings; }
sv::layer_kind                   // layout | scene_3d | scene_2d | ui — composited in order, each over the ones before it
                                 //   a `layout` layer renders a whole tree INTO this view's texture; that is the recursion in the model
                                 //   scene_2d draws nothing (shaped-core has no 2D renderer); ui is not wired yet
sv::layer_blend                  // replace | over (premultiplied) — scene_3d is forced to replace until the raygen writes alpha
sv::primary_scene_3d(v) / sv::ensure_scene_3d(v)  // -> layer const* / layer& — the view's first traced layer, appended on demand
sv::refresh_policy               // { float rate; } — fraction of the loop's rate: 1 every frame, 0.5 every second, 0 only on invalidation
sv::temporal_input               // { u64 id; optional<vec2i> resolution; pixel_format; u64 reset_hash; } — unset resolution = the view's own
sv::temporal_inputs_of(view_data) -> vector<temporal_input>  // what the view declared, PLUS one rgba32_float accumulator per scene_3d layer
sv::temporal_id::accumulation(layer) -> u64   // the id a traced layer accumulates under; >= caller_range_end, so a caller cannot collide
sv::view_id                      // stable identity across frames; view_id::from_string("main", seed=0) — keys everything a view keeps
                                 //   the WHOLE string is hashed, `##` included, so "angle##0" and "angle##1" are two views
sv::view_index                   // enum class : u32 — a view's slot in `viewer_definition::views` THIS frame; not an identity, so never persist one
sv::display_name_of("angle##0")  // -> "angle"; the id up to its first ##, and what a view's display name defaults to
sv::push_id_seed(seed, "name") / (seed, i64 n)  // fold a scope into a seed; what frame::push_id does, for a caller minting ids by hand
sv::camera                       // { pos3d position; quat_d orientation; perspective_projection projection; } — double-precision pose + projection
sv::camera::looking_at(eye, target, up=+y)     // -> camera at eye aimed at target (static factory); default projection
sv::camera::orbiting(target, distance, azimuth, elevation)  // -> camera orbiting target, looking inward (azimuth around +y, elevation off the horizon)
sv::camera::look_rotation(eye, target, up=+y)  // -> quat_d aiming from eye at target (static); cam.look_at(target, up=+y) sets it from position
cam.basis()                      // -> camera_basis { vec3d right, up, forward } — the world axes a screen-space drag is expressed in
sv::perspective_projection       // { angle_d vertical_fov; f64 aspect_ratio; f64 near_plane; } — the only projection kind for now
sv::camera_gpu::from(cam)        // -> camera_gpu (the GPU basis: forward/right_scaled/up_scaled); aspect comes from projection.aspect_ratio
sv::render_settings              // { int samples_per_pixel, max_bounces; } — view-wide integration controls (no light/sky: those are on the view)
sv::scene_item                   // { scene_item_kind kind; mesh_id mesh; instance_id instance; hash128 permutation; tg::affine_transform3f transform; } — triangle_mesh only for now
                                 //   mint one with resources.acquire_scene_item(mesh); the three ids have to come from ONE material resolution
                                 //   build the placement with tg's factories (make_rotation(quat), make_translation(vec), make_from_linear_mat(mat3)) and tg::compose
                                 //   default-constructs to the identity; the renderer packs its linear part + translation into the TLAS's row-major 3x4
sv::area_light                   // { pos3f center; vec3f half_extent_u, half_extent_v; vec3f emission; } — a world-space rect emitting along cross(half_extent_u, half_extent_v); one typed list per light kind on the view
                                 //   emission has no default (it is -1): set it, or the first use warns to stderr
sv::area_light_gpu::from(light)  // -> area_light_gpu { vec3f center, u, v, emission, normal; } — the rect in GPU lane layout (u/v are the half-extents, normal = cross(u, v))
sv::background                   // { vec3f sh[16]; } — order-3 RGB SH environment a missed ray sees (the flat and pt misses both reconstruct radiance from it)
sv::background::uniform(radiance)              // -> background — the same radiance in every direction (band 0 alone)
sv::background::gradient(zenith, nadir)        // -> background — vertical (+y) gradient, exact: zenith straight up, nadir straight down, their average on the horizon
sv::background::sun(direction, radiance)       // -> background — soft lobe peaking at exactly `radiance` along `direction` (normalized for you); truncated clamped cosine, so it leaves a floor (3/34 of the peak across, 1/17 behind) and dips slightly negative in the ring between
sv::background::daylight() / ::studio()        // -> background — presets: blue sky + warm ground + soft sun / neutral gray brighter overhead
bg.combined_with(other) / bg.scaled(factor)    // -> background — SH is linear, so environments superpose and scale; how gradient + sun compose into a preset
sv::background_gpu::from(bg)     // -> background_gpu { vec4f sh[16]; } — GPU lane layout (each coeff widened to a vec4); the miss's Background cbuffer at b1
sv::pbr_material                 // { vec3f base_color, emissive; float metallic, roughness; } — pbr_raytrace_routine's vocabulary, flat per-triangle
                                 //   the path tracer shades through sv::material instead; the same four fields are attributes of the builtin `pbr` type
```

## Layout — the tree a view is filled with

```cpp
sv::layout_tree                  // { vector<layout_node> nodes; } — one flat pool per frame; every layout layer names a root in it
sv::layout_node_id               // enum class : u32 — a node's slot in a tree; t[id] indexes, sv::invalid_node is the "no node" sentinel
t.add_container(parent, style, grid) / t.add_leaf(parent, leaf, style) / t.add_relative(parent, placement, style)
                                 //   all -> layout_node_id; parent == sv::invalid_node makes a fresh root
sv::layout_kind                  // grid | relative | leaf              — one tiling kind; grid_params says linear, fixed or automatic
sv::layout_node                  // { layout_kind kind; box_style style; grid_params grid; relative_placement placement; vector<layout_node_id> children; layout_leaf leaf; }
sv::box_style                    // { box_insets margin; int border; vec4f border_color, background_color; box_insets padding; int spacing; }
                                 //   outside-in: margin, the border band, padding, then the content box children tile; spacing separates siblings
                                 //   a border with no alpha still reserves its width — that is how an invisible gutter is spelled
                                 //   background_color fills the whole border box, under the border and through the padding — padding has no color of its own
                                 //   it draws before the border and before every child, so it is also what colors the gutters between them
sv::box_insets                   // { int left, top, right, bottom; } — constructors rather than designated initializers
                                 //   `.padding = 8` (all four), `= {8, 4}` (horizontal, vertical), `= {1, 2, 3, 4}` (left, top, right, bottom)
                                 //   ::all(v) / ::symmetric(horizontal, vertical) are the named spellings of the first two
sv::relative_placement           // { pos2f position; vec2f size; vec2i position_offset, size_offset; } — fraction of the parent's content box, plus pixels
                                 //   a `relative` node is OUT OF FLOW: siblings tile as if it were absent, and it draws in front
sv::layout_leaf                  // { vector<view_index> views; vector<post_process> post_processes; fit_mode fit; sampler_mode sampler; bool allow_zoom; float zoom; pos2f zoom_center; }
sv::fit_mode                     // stretch | native                    (todo: fill, contain, crop)
sv::sampler_mode                 // nearest | linear                    (nearest + zoom is a pixel-exact readout)
sv::post_process                 // { post_process_kind kind; float split; bool horizontal; int separator_width; vec4f separator_color; } — none | wipe
sv::grid_params                  // { optional<int> cols, rows; float target_aspect, empty_penalty; }
                                 //   set both -> a fixed grid; set neither -> the auto-grid; set one -> the other follows the child count
                                 //   children fill row-major, so {.cols = 3} is rows of three, {.cols = 1} a column and {.rows = 1} a row

sv::resolve_layout(tree, root, rect) -> layout_solution   // { vector<resolved_item> items; } in DRAW order — a node's background, then its borders, then its children
sv::resolve_grid_dims(n, area_aspect, p) -> grid_dims     // the pinned / derived rule above; both counts >= 1
sv::auto_grid_dims(n, area_aspect, p) / subdivide_grid(rect, cols, rows, spacing) / subdivide_linear(rect, n, horizontal, spacing)
sv::inset(rect, insets) / border_box(rect, style) / content_box(rect, style) / border_bands(rect, width, out)
sv::has_visible_border(style) / has_visible_background(style)   // whether either is worth a draw; a border needs a width AND alpha, a background only alpha
```

The **default auto-grid** keeps cells near square: 1 fills, 2 side-by-side, 3 in a row, 4 as a 2x2, 5 as 3x2 (landscape).
The solvers are pure and unit-tested without a device ([tests/layout-test.cc](tests/layout-test.cc)).

## Render plan — a frame flattened into recording order

`build_render_plan` is a **pure function** holding no GPU resources, so every invariant below is testable headless ([tests/render-plan-test.cc](tests/render-plan-test.cc)).

```cpp
sv::build_render_plan(def, output_size, frame_index, history) -> render_plan
sv::render_plan                  // { vector<plan_target> targets; vector<plan_trace> traces; vector<layout_draw> draws;
                                 //   vector<hit_region> hit_regions; vector<plan_diagnostic> diagnostics; vector<view_id> reachable; }
plan.draws_of(target)            // -> span<layout_draw const>; that target's whole pass, in draw order
plan.validate()                  // -> bool; every draw's source target index is BELOW the target it writes
sv::plan_target                  // { view_id id; view_index view; vec2i resolution; pixel_format; bool is_output; bool refresh; } — dependency order, output last
sv::plan_trace                   // { view_id id; view_index view; u8 layer; vec2i resolution; } — one per scene_3d layer
sv::layout_draw                  // { draw_kind kind; aabb2i dst_rect; vec4f color; draw_source primary, secondary; sampler_mode; layer_blend; float opacity; post_process; layout_node_id node; }
sv::draw_kind                    // background | border | view | wipe
                                 //   background and border are both flat-color fills through one pipeline; the plan keeps them apart so it reads as a description
sv::draw_source                  // { draw_source_kind kind; u32 index; aabb2f uv; } — target | trace; uv is where fit + zoom already resolved to
sv::hit_region                   // { view_index view; view_id id; layout_node_id node; aabb2i window_rect; vec2f scale, offset; u32 parent; u32 order; }
                                 //   `parent` is an index into hit_regions, NOT a node — one region per reference, so two can share a node
sv::pick_hit_region(regions, pt) // -> u32 into hit_regions; painter's order via the parent links, sv::invalid_hit_region when nothing is hit
sv::view_history                 // { map<view_id, view_history_entry> entries; } — what the renderer already holds, so refresh is decided purely
sv::max_layout_depth / sv::max_plan_targets   // guards; exceeding them reports a diagnostic rather than asserting
```

Three properties worth knowing:

- **Traces hoist above every pass.** No trace reads a target in the same frame, so a three-level nest still costs exactly one dispatch to raster alternation, whatever the depth.
- **A shared view renders once.** Two leaves naming the same view get one target, sized by the *largest* rect that asks for it — not the first, which would make the image depend on sibling order.
- **A cycle degrades.** It reports a `plan_diagnostic` and drops that one leaf; every sibling still renders.
  A view tree is frequently data, so it must not assert.

## Material data on the GPU — attributes, and the block a permutation reads

```cpp
m.attributes.acquire(mesh_attribute)   // -> attribute_id; uploads the bytes into a persistent buffer, keyed on attribute.hash
m.attributes.get(id)                   // -> attribute_record {buffer<byte> data; attribute_format format; attribute_frequency frequency; element_count;}

m.acquire_instance(resolved, layout)   // -> instance_id, content-keyed on resolved.parameter_key; layout from generate_material_shader over `resolved`
m.get_instance(id)                     // -> instance_record {i32 size_bytes; vector<instance_slot> slots; buffer<byte> parameters; vector<byte> uploaded;}
m.contains_instance(id) / m.instance_count()
sv::instance_slot                      // { material_slot_kind kind; i32 offset, size_bytes; vector<byte> constant; attribute_id attribute; u32 element_stride; texture_id texture; }
m.build_instance_parameters(record)    // -> vector<byte> for THIS epoch; acquires every descriptor and texture index it writes

m.acquire_scene_item(sv::mesh)         // -> scene_item; geometry + BLAS, the material resolved, its permutation compiled, its block resolved
                                       //   material_id::invalid falls back to sv::default_material, so a mesh always draws
m.describe_instance(cmd, mesh_id, instance_id)  // -> instance_gpu, the per-item record a closest-hit reads by InstanceID()
                                       //   rebuilds the block for THIS epoch, uploads it on cmd only if it changed, and mints all four indices
sv::instance_gpu                       // { u32 param_buffer, param_offset, vertices, indices, is_indexed; } — 32 bytes, mirrors sv::instance
```

A block holds, at the offsets `material_parameter_layout` names: a constant inline, an `sv::attribute_desc`
(`{bindless index, 0, element stride}`) per mesh-sourced attribute, and a bindless index per sampled texture.

Gotchas:

- **`gpu_resource_manager::create` requires `textures_2d` and `buffers`** in the bindless config, whatever else it declares.
  A sampled texture is acquired into the first; geometry, attributes and parameter blocks into the second.
- **Every index in a block is THIS EPOCH's**, never a pinned one — which is why a block is rebuilt per frame rather than cached across frames.
  That is what makes the access declaration correct by construction: nothing a hit reads reached the GPU without an acquire.
- **`describe_instance` must be called on the list that traces with it, and before `freeze()`** — it is where those indices are minted.
- **The block's bytes are the epoch's; its buffer is not.** The record owns a persistent buffer and re-uploads only when the bytes actually differ.
  A fresh transient buffer per frame would mint a new descriptor every frame, leaving the staging group permanently dirty.
  Every trace then re-mints the whole 8752-descriptor table, which exhausts the heap in seconds.
- **The layout and shader key must come from ONE `generate_material_shader`** over that same resolved material, or the block is filled at offsets the shader does not read.
- **A sampled texture must already be resident** — a `texture_id` on a mesh is one the caller acquired.
- **The block is zero-filled first**, so alignment padding is stable and one material does not upload as two different blobs.
- **`is_indexed` rides on the instance, not the frame.** Geometry layout is a property of the mesh, and a view may hold an indexed and a non-indexed one at once.
- **`instance_gpu` is a byte layout**, not a description of one — keep it in lockstep with `sv::instance` in `shaders/material_runtime.hlsli`.

## Materials — a type, an instance, and the frequency chain

`material/material_library.hh` is the front door; it pulls in `material.hh` and `material_type.hh`.

```cpp
sv::material_type                // { string name; vector<material_signature_entry> signature; string shader; hash128 hash; }
sv::material_type::create(name, signature, shader)   // -> hashes all three; asserts a name declared twice, and a default that is not its format's size
t.find("roughness")              // -> material_signature_entry const*, null if the type does not read it
sv::material_signature_entry      // { string name; attribute_format format; vector<byte> default_value; bool is_final; }
sv::material_signature_entry::of("roughness", 0.5f)   // -> format deduced via attribute_format_of<T>; trailing bool pins it final
sv::material                     // { string name; material_type_id type; vector<material_attribute_binding> overrides; hash128 hash; }
sv::material::create(name, type, overrides)          // -> hashes type + overrides; validates NOTHING (it cannot see the type)
sv::material_signature_entry::of_rotation("tangent_frame", q)  // -> the same, blended as a QUATERNION across the hit triangle
                                 //   requires an f32x4; corners are hemisphere-aligned before the blend, which sv::interpolate_f4 would not do
sv::attribute_interpolation      // linear | rotation — a property of the DECLARATION, so it rides the type hash into the permutation key
sv::material_attribute_binding::of("roughness", 0.2f)          // -> a constant binding; trailing bool makes it final
sv::material_attribute_binding::of_texture(name, sample)       // -> a uv-sampled binding, from a texture_sample_source

sv::material_library::create()   // -> an empty library; register_builtin_material_types(lib) adds `openpbr`, `pbr` and `unlit`
lib.register_type(type)          // -> material_type_id, content-addressed; asserts two DIFFERENT types under one name
lib.acquire_type("pbr")          // -> optional<material_type_id>;  lib.get_type(id) -> material_type const&
lib.acquire(material)            // -> material_id, content-addressed; HERE every binding is validated against the type
lib.acquire("gold")              // -> optional<material_id>;  lib.get(id) -> material const&
sv::builtin_material::openpbr / pbr / unlit          // the names the builtins register under
                                 //   openpbr: the OpenPBR Surface subset (20 attributes) — the type to author against
                                 //   pbr: glTF metallic-roughness, projected onto the same surface; unlit: emission only
sv::default_material(lib)        // -> material_id — an unbound `pbr`; what a mesh naming material_id::invalid draws with
sv::set_acquire_material_library(provider)           // the context-style hook; {} clears it, the default registers the builtins
sv::set_acquire_shader_library(provider)             // the same shape for the SHADER library; the default registers sv's + sr's packages + DXC
sv::acquire_shader_library()     // -> result<slib::shader_library*>, process-wide — what a GENERATED permutation compiles through
sv::acquire_material_library()   // -> result<material_library*>, created once per process and shared

sv::resolve_material(type, material, mesh)           // -> resolved_material; the pure form
sv::resolve_material(lib, material_id, mesh)         // -> the same, resolving the id through the library
sv::resolved_material            // { material_type const*; material const*; vector<resolved_attribute>; hash128 permutation_key, parameter_key; }
sv::resolved_attribute           // { string_view name; attribute_format format; material_frequency frequency;
                                 //   span<byte const> constant; mesh_attribute const* attribute;
                                 //   texture_sample_source const* sample; mesh_attribute const* uv; }
                                 //   exactly one payload is live, `frequency` says which; `uv` rides along with `sample` and is never null when it isn't
sv::material_frequency           // material_type < material < mesh_instance < mesh_attribute < material_texture < mesh_texture
```

**The enum order IS the precedence** — a value varying more finely beats one varying more coarsely, and `resolve_material` compares nothing else.
`is_final`, on a declaration or a binding, stops the walk there so no finer frequency overrides it — which is how a material refuses a mesh's roughness texture.
A `final` texture binding stops it even when its own sample was skipped, so the mesh's texture never wins by the back door.
A candidate that cannot be used is skipped rather than fatal: a mesh attribute at the wrong format, or a texture whose `uv_attribute` the mesh does not carry, simply loses its turn.
So every declaration resolves to something and a mesh carrying none of what a type asks for still draws.

**The two keys are the point.**
`permutation_key` covers only the SHAPE of the resolution (which rank won, the geometric frequency, the uv attribute and sampler) plus the type's hash, so gold and copper share one generated shader.
`parameter_key` covers the resolved values (constant bytes, texture ids, each mesh attribute's own hash), and keys the per-instance slot they are written into.
Only a texture sample forces a second permutation, which is what makes "the shader stays the same" mechanical rather than aspirational.
Neither is what a shader cache is keyed on: `material_shader_key` folds `permutation_key` together with the options it is generated under, and a `scene_item::shader_key` is that.

Gotchas:

- **Every payload on a `resolved_attribute` BORROWS** — from the library, the material or the mesh.
  A resolved material must not outlive any of them.
- **Nothing is ever evicted from a library.** A `material_id` is written into GPU memory outliving its frame, so the ids have to stay meaningful; this is why it is not an `impl::lru_pool`.
- **`material::create` validates nothing** against the type, because it cannot see one.
  `material_library::acquire` is where a binding naming an undeclared attribute asserts.
- **A `material_type::shader` is a FRAGMENT, not a shader.** It reads each signature attribute as an already-initialized local and assigns `surface`; the generator writes everything around it.
- **`surface` is OpenPBR's parameter set** (`sv::surface`, `shaders/openpbr.hlsli`), not a metallic-roughness struct.
  Every type writes that one vocabulary, which is what lets the integrator evaluate a single layered BSDF whatever the material was authored as.
- **The generator emits `#define SV_ATTR_SUPPLIED_<name> 0/1` per attribute**, one constant per permutation.
  It separates "something supplied this" from "the declaration's default came through", which a fragment cannot tell and the tangent frame depends on:
  an unsupplied frame must fall back to the geometric one rather than trust the identity rotation, which points at object-space +z.

## Material shaders — one permutation, generated

`material/shader_generator.hh`; `shaders/material_runtime.hlsli` is the hand-authored half it is written against.

```cpp
sv::generate_material_shader(resolved, opts = {})  // -> generated_material_shader {string source; material_parameter_layout layout; vector<sg::sampler> samplers; hash128 key;}
                                 //   samplers[i] is what `sv_sampler_i` must be bound to; the text names a register and nothing else records the state
sv::hlsl_type_of(format)         // -> "float" / "float3" / "uint2" / ...; EMPTY for a format the generator does not support
sv::material_shader_options      // { entry_point = "sv_evaluate_material"; runtime_include; epilogue_include; bindless_config const*; }
                                 //   epilogue_include is emitted AFTER the entry function, for code that CALLS it
{.epilogue_include = "pt_material_hit.hlsli"}   // -> a full DXR closest-hit for this permutation, not just the material function
sv::material_shader_key(permutation_key, opts)  // -> hash128 — what `g.key` is, without generating anything

resources.shaders                // the cache a viewer uses: gpu_resource_manager owns one, in the context's preferred format, over cfg.bindless
sv::material_shader_cache::create(format, opts = {})   // one compiled closest-hit per permutation; `opts` is COPIED and is part of the key
sv::material_shader_cache::hit_entry_point / hit_epilogue_include   // "PtClosestHit" / "pt_material_hit.hlsli"
cache.generation_options()       // -> material_shader_options borrowing from the cache — what to pass material_shader_key
cache.acquire(resolved)          // -> material_permutation const& {hash128 key; layout; vector<sg::sampler> samplers; async_compiled_shader shader; string source;}
cache.find(shader_key)           // -> material_permutation const*, null if nothing acquired it;  cache.count()
sv::material_parameter_layout    // { vector<material_slot> slots; i32 size_bytes; } — the per-instance block, 4-byte aligned
sv::material_slot                // { string name; material_slot_kind kind; i32 offset, size_bytes; attribute_format format; i32 attribute_index; }
sv::material_slot_kind           // constant | attribute_descriptor (an sv::attribute_desc) | texture_index (a u32 into the 2D table)

slib::shader_library::compile_source(src, stage, entry, format, {.include_dir = "sv_shaders"})  // -> sg::async_compiled_shader
```

The generated source is, in order: the runtime include, only the bindless tables this permutation touches, one `SamplerState` per
distinct sampler, then the entry function.
That function declares one local per signature attribute — a parameter-block load, a barycentric interpolation, or a uv sample —
and then runs the type's fragment verbatim over them.
The loads run in a NESTED BLOCK, so the parameter buffer, an attribute's descriptor and a sampled uv never reach the fragment's scope.
`g.key` is `material_shader_key(resolved.permutation_key, opts)`: the resolution's shape AND how these options spell it.
Two calls agreeing on that pair generate byte-identical source, and nothing else may share their cache entry — a second cache over different bindless budgets gets its own.

Gotchas:

- **The layout is the contract, and both sides read it.** The shader loads slot `offset`; the CPU fills slot `offset`.
  Nothing recomputes it independently, which is why it comes back with the source rather than being derivable.
- **A sampled attribute takes TWO slots** — the texture index, and the `sv::attribute_desc` for the uv set it samples through, named `"<attribute>.uv"`.
- **The geometric frequency is part of the permutation**, so a descriptor carries one stride and the generated code emits the index math.
  Three strides plus a runtime branch would be the other trade; see `material_runtime.hlsli`.
- **`SampleLevel`, never `Sample`** — a ray tracing hit shader has no derivatives to pick a mip from.
- **Every bindless index is wrapped in `NonUniformResourceIndex`**, because it varies per instance within a wave.
- **Scalars and vectors of f32 / i32 / u32 only.** A matrix or a narrow / 64-bit scalar asserts rather than emitting code that will not compile.
- **The runtime lives in `namespace sv`**, so an attribute may be named `params`, `desc` or `uv` — the fragment shares its scope with the attribute names, `surface` and `ctx`, and nothing else.
  `sv_` survives only for `sv_sampler_i` and the entry point, which are file scope and cannot be namespaced without changing what reflection reports.
- **An attribute name is pasted in as a local**, so `material_type::create` rejects one that is not a plain identifier, is an HLSL keyword or builtin type, starts with `sv_`, or is `surface` / `ctx`.
  Rejected rather than sanitized: the type's own fragment is written against the declared name.
- **A generated permutation does not hot-reload on an include edit** — the key hashes the resolution and the options, not the include's contents.

## Mesh authoring — geometry + what a material reads

One header per part — `mesh.hh` pulls in `triangle_geometry.hh`, `mesh_attribute.hh`, `mesh_flags.hh` and `mesh_texture.hh`.

```cpp
sv::mesh                         // { string name; triangle_geometry geometry; vector<mesh_attribute> attributes; affine_transform3f transform;
                                 //   material_id material; mesh_flags flags; vector<mesh_texture> textures; }
m.is_visible()                   // -> bool (flags.has(mesh_flag::visible)); the rest of a mesh is plain public data
sv::triangle_geometry            // { pinned_data<pos3f const> positions; pinned_data<u32 const> indices; hash128 hash; } — raw or indexed, one type
sv::triangle_geometry::create_from_triangles(triangles)            // -> from a range of tg::triangle3f; the pin is reinterpreted onto the positions, never copied
sv::triangle_geometry::create_from_indexed_triangles(pos, indices) // -> 3 indices per triangle, each < positions.size()
g.is_indexed() / g.is_empty() / g.vertex_count() / g.triangle_count()  // triangle_count follows the index buffer when there is one
sv::triangle_data::from(g) / sv::indexed_triangle_data::from(g)    // -> the mesh_manager payload, same hash, sharing the pin (asserts on the wrong layout)
sv::mesh_attribute               // { string name; attribute_format format; attribute_frequency frequency; pinned_data<byte const> data; hash128 hash; }
sv::mesh_attribute::create(name, frequency, elements)       // -> mesh_attribute; format deduced from the element type, bytes pinned + hashed
sv::mesh_attribute::create_value(name, x)                   // -> a per_instance attribute of exactly one element — what a per-mesh material input is
a.element_count() / a.elements_as<tg::vec3f>()              // -> isize / span<T const> (asserts if T is not what format names)
a.holds<T>() / a.value_as<T>()                              // -> bool / T; holds<T>() is what a material asks first, value_as reads the per_instance element
sv::scalar_type                  // i8 i16 i32 i64 | u8 u16 u32 u64 | f32 f64 | boolean (1 byte, 0/1) — the complete set; scalar_type_size(t) -> i32 bytes
sv::attribute_format             // { scalar_type scalar; int dim0, dim1; } — scalar + dimensionality, so every scalar/shape combination exists
                                 //   dim0 alone is a vector's component count; for a matrix dim0 is its rows and dim1 its columns
attribute_format::of_scalar(s) / of_vector(s, dim) / of_matrix(s, rows, cols)  // both dims in 1..4; of_matrix is column-major, matching tg::mat<C, R, T>
f.size_bytes() / f.component_count() / f.is_scalar() / f.is_vector() / f.is_matrix()
sv::attribute_format_of<T>       // the format of an element type — scalars and tg vec / pos / comp / mat over them
sv::attribute_frequency          // per_instance (exactly 1 element, create asserts) | per_vertex | per_corner (3 per triangle, in triangle order) | per_triangle
                                 //   per_edge is RESERVED and create asserts on it
sv::mesh_flag / sv::mesh_flags   // visible | casts_shadow | receives_shadow (cc::flags); mesh_flags_default is all three — the EMPTY set draws nothing
sv::texture_sample_source        // { texture_id texture; string uv_attribute; sg::sampler sampler; } — everything a sample needs but what it is FOR
sv::mesh_texture                 // { string name; texture_sample_source source; } — a sample offered under the attribute name it fills
sv::material_id                  // thin handle naming ONE material definition, minted by material_library::acquire
```

The material is what gives the three lists their meaning: it decides which attribute names it samples, which texture slots it binds, and how the flags change what it emits.
Per-mesh values are attributes too, at `per_instance` — one list, so a renderer can pack every instance's values into one buffer and index it by instance.
So a mesh may carry data no material uses and miss data another would want — a material falls back rather than failing.
The mesh offers no by-name lookup, deliberately: a material resolves the names it wants once, into whatever binding table it draws from, rather than scanning strings per draw.
Copying a mesh shares the pinned payloads (a refcount bump), so passing one around is cheap.
Nothing renders an `sv::mesh` yet: the renderer still consumes `sv::scene_item` (ids into the managers), and the bridge is `triangle_data::from(triangle_geometry)`.
`mesh.material` is likewise authored and not yet drawn — `sv::resolve_material` says what it means, but no shader is generated from the answer.
The seeds behind every content key live in `impl/content_hash.hh`, so a geometry and the payload it is uploaded as agree on one key instead of caching the same bytes twice.

## Camera control — orbit and first-person

```cpp
sv::orbit_state                  // { pos3d target; f64 distance; angle_d azimuth, elevation, vertical_fov; }
o.to_camera()                    // -> camera (camera::orbiting plus the fov); orbit_state::from_camera(cam, distance) is the lossy inverse
sv::orbit_camera_controller      // { orbit_state orbit; camera_controller_config config; }
c.handle(sr::input_event) -> bool  // apply one event ALREADY routed to this view; true if the camera actually moved
c.camera() / c.is_dragging()     // the resulting camera / whether a drag is in progress
sv::camera_controller_config     // { f64 orbit_degrees_per_pixel, pan_units_per_pixel, zoom_per_wheel_tick, min_distance, max_distance; angle_d max_elevation; }
```

Left drag orbits, middle drag pans the target (scaled by distance, so one drag covers the same screen fraction at every zoom), the wheel scales the distance geometrically.
It is **event-driven and time-free** — every input carries the motion it caused, so there is no `update(dt)` to schedule and a view that gets no input produces none.
`sv::viewer` drives one per view for you; a caller running its own event pump (see `viewer-window-manual-test.cc`) just feeds it `wsys->events()`.

```cpp
sv::fps_state                    // { pos3d position; angle_d yaw, pitch, vertical_fov; } — yaw around +y, 0 looks along +z
s.forward() / s.to_camera()      // -> vec3d view direction / -> camera; fps_state::from_camera(cam) is the exact inverse (roll dropped)
sv::fps_camera_controller        // { fps_state pose; fps_camera_controller_config config; }
c.handle(sr::input_event) -> bool  // apply one event ALREADY routed to this view; true only for a look — a key just arms update()
c.update(dt) -> bool             // integrate the held keys over dt seconds; true if the camera moved. Call EVERY frame, event or not
c.camera() / c.is_looking() / c.is_moving()
c.release_input()                // drop every held key + the look — for a view that lost focus and will never see the key-up
sv::fps_camera_controller_config // { f64 look_degrees_per_pixel, move_units_per_second, fast_multiplier, slow_multiplier, speed_per_wheel_tick,
                                 //   min/max_move_units_per_second; angle_d max_pitch; bool look_requires_button; }
```

Right-drag looks, W/A/S/D move along the view, E/Q rise and fall along world +y, shift accelerates, ctrl slows, and the wheel retunes `move_units_per_second` instead of moving.
Free-flying — no gravity or collision, and forward follows the pitch, so W into a raised view climbs.
Unlike the orbit controller it **integrates over time**, hence the two halves: `handle` for events, `update(dt)` once per frame for the motion they imply.
Set `look_requires_button = false` once the caller has captured the cursor (`sr::window::set_relative_mouse_mode`), so every motion turns the view.

## Resources by id — the managers

```cpp
sv::gpu_resource_manager::create(ctx, cfg)  // named ctor; cfg = { manager_config meshes, materials, textures; bindless_config bindless }
sv::mesh_manager::create(ctx, cfg)     // cfg = manager_config { resource_budget budget }; ctx must outlive it

// What you hand a manager: an owning cc::pinned_data payload + the cc::hash128 that identifies it.
sv::triangle_data          // { pinned_data<pos3f const> positions; hash128 hash; } — non-indexed list, 3 positions per triangle
sv::indexed_triangle_data  // { pinned_data<pos3f const> positions; pinned_data<u32 const> indices; hash128 hash; } — 3 indices per triangle
sv::material_data          // { pinned_data<pbr_material const> materials; hash128 hash; } — one per triangle
T::create(range…)          // pins (moving an owning rvalue in, deep-copying a borrow) + hashes now (XXH3-128); call once at authoring time, not per frame
sv::hash_bytes_of(span) / sv::combine_hashes(a, b)  // the hashing primitives, if you key content yourself

mesh_manager::acquire(triangle_data) -> mesh_id          // O(1) if resident; else uploads + builds a non-indexed BLAS
mesh_manager::acquire(indexed_triangle_data) -> mesh_id  // same, but an indexed BLAS: PrimitiveIndex() order follows the index buffer
sv::mesh_record          // { buffer<pos3f> vertices; buffer<u32> indices; bool is_indexed; isize triangle_count; blas_handle blas; }
material_manager::acquire(material_data) -> material_set_id  // O(1) if resident; else uploads (one pbr_material_gpu per triangle)
                                                             //   pbr_raytrace_routine's path only — the path tracer reads a per-instance block instead
manager.get(id) / get_ptr(id) / contains(id)        // resolve an id back to its record (get_ptr also LRU-touches)
manager.set_limits(max_bytes, max_idle_epochs)      // change the budget at runtime (0/‑1 = unbounded/never)
manager.used_bytes() / count() / evict(id)          // current residency; manual drop
resources.advance_to(epoch)                         // reclaim + advance; IDEMPOTENT, so every window's draw path may call it and the first one pays
resources.current_epoch()                           // -> sg::epoch — what it last advanced to
sv::mesh_id / material_set_id / instance_id / attribute_id / tlas_id / texture_id / buffer_id   // enum class : u32; ::invalid == u32(-1) (ids mint from 0)
```

The managers ride on `sv::impl::lru_pool<Id, Record>`, the reusable id-pool.
It mints ids, tracks each record's byte size and last-used epoch, and evicts on the idle timeout or the byte budget, least-recently-used first.
It never evicts this frame's working set; `advance_to` in its header states that rule exactly.
It is content-addressed: records go in under the caller-supplied `cc::hash128`, so `acquire` is O(1) and never re-uploads content it already holds.
A manager never hashes anything itself, so hash load stays where the caller schedules it and never lands inside a per-frame acquire.

### Bindless tables — declared by sv, owned by the manager

```cpp
sv::bindless_table          // enum class : u8 — textures_1d / _1d_array / _2d / _2d_array / cube / cube_array / _3d / buffers (+ count_)
sv::name_of(table)          // -> cc::string_view — the shader-visible binding name: gBindlessTextures2D, gBindlessBuffers, …
sv::space_of(table)         // -> u32 — one register space per table, so a category needs no register-offset math
sv::bindless_table_budget   // { bindless_table table; u32 count; }  — count 0 OMITS the table; a non-zero count < 2 ASSERTS (sg reads 1 as a scalar binding)
sv::bindless_config         // { cc::vector<bindless_table_budget> tables = default_bindless_tables(); }
sv::make_bindless_bindings(cfg)  // -> cc::vector<sg::binding> — the hand-declared layout; pure, so it needs no context

m.acquire_texture(table, raw_view) -> sg::bindless_index   // THIS EPOCH ONLY; asserts when frozen or when the table is not declared
m.acquire_buffer(raw_view) -> sg::bindless_index           // the same for the byte-address table
m.pin_texture(table, raw_view) -> sg::bindless_element_handle  // pinned; h->index() outlives the epoch. NOTHING in sv uses one
m.pin_buffer(raw_view) -> sg::bindless_element_handle          // the same for the byte-address table
                                            //   a pin needs no unlock, and is recorded for the access declaration like an acquire
m.lock() / unlock() / is_locked()           // refuse acquires while a snapshot is bound — the manual pair
m.freeze() -> sv::bound_resources           // RAII: locks, snapshots, unlocks when it dies. SEVERAL per epoch are fine
bound.group() / bound.layout()              // -> the group to bind, and the layout a pipeline composes it as one of its groups
bound.elements(table)                       // -> span<u32 const> — this epoch's acquired indices, for declare_array_*_access (which dispatch ASSERTS on)
bound.declare_raytracing_access(cmd)        // declares EVERY declared table for the next dispatch_rays, empty ones included
m.bindless_layout()                         // -> the same layout, without taking a snapshot
m.has_table(table) / m.table_capacity(table)

// textures + the follow-up work their policy asks for
sv::texture_data::create(pixels, format, w, h, mip_count=1)  // pins + hashes; the SHAPE is part of the key, not just the bytes
m.acquire_texture(texture_data) -> sv::texture_id   // O(1) if resident; else creates the FULL chain, uploads what was supplied, queues the rest
m.textures.get_ptr(id) -> texture_record const*     // { texture_2d texture; residency state; i32 uploaded_mips, total_mips; }
sv::residency                // pending | base_resident | complete — an id never blocks, so what varies is how good it is yet
m.record_pending_work(cmd) -> i32   // records what THIS epoch's budget allows, oldest first; returns dispatches spent
m.pending_work_count()              // -> isize — resources still waiting for their follow-up
// config: { texture_policy textures_policy = {.generate_mips = true}; work_budget work = {.max_dispatches_per_epoch = 16} }
```

**The two budgets are different things.** Bytes in flight are sg's to schedule (`ctx.stream.set_upload_ratio`, per-handle priorities, aging).
What `work_budget` bounds is the work that runs *after* a resource lands — mip generation today — and leaving it unbounded is what produces the microstutter.
A chain is never split across epochs: a partially generated one would read as complete while its tail is uninitialized.

The layout is hand-written rather than reflected, so the manager is constructible before any shader compiles — a shader matches the names above.
The lock lives here rather than on `sg::bindless_array` because the invariant spans every array over one staging group.
What keeps an index valid is sg's reclaim rule, not the lock: a full array reclaims only what was NOT acquired this epoch, and never a pinned element.
See the header's TODO block for what that leaves open.
An index that must outlive its epoch is *pinned*, and the type system enforces it: a `bindless_index` cannot be stored where a persistent one belongs.
**sv pins nothing.** Every index a hit reads is acquired for the epoch that records with it, which is what makes `bound.declare_raytracing_access` complete by construction rather than by remembering.

## Rendering — the view_renderer + routines

```cpp
// Both the FRAME's job — once, before the first view resolves its ids or reaches for its accumulator.
resources.advance_to(ctx.current_epoch())
store.begin_frame(u64(ctx.current_epoch()))               // reclaims idle view textures; skipping it only means nothing is reclaimed

// One view -> the texture the store keeps under its id. A convenience for a caller tracing a single view with no plan.
sv::view_renderer::execute(cmd, view_data, resources, store) -> sg::texture_2d
                                                          //   traces into a PERSISTENT rgba16f target keyed by v.id, sized from v.resolution
                                                          //   blends in place, restarting whenever the traced image changed at all
                                                          //   the store owns it: holding it past the next execute/begin_frame for that id is invalid
                                                          //   assumes ONE traced layer per view; the plan path below is per (view, layer)
store.accumulated_frames(view_id) -> u32                  // frames accumulated so far; 0 after a restart. For tests and debug overlays
store.min_accumulated_frames(id) / .is_accumulation_converged(id, frames = {})
                                                         // the same folded over EVERY traced layer — what convergence actually means

// The whole frame, from its plan: every trace first, then one pass per refreshing target, the output last.
sv::viewer_renderer::execute(cmd, def, plan, resources, store, output)   // output = a sg::color_target, e.g. rt.cleared(clear_color)
                                                          //   `store` MUST be the one `plan`'s view_history was read from
                                                          //   nothing writes the gaps a layout leaves — pass output.cleared(...)
                                                          //   an empty plan still opens no pass; an empty def leaves the clear alone to land

// The leaf routines they drive — each an sg::render_routine<> (everything that traces/draws is a routine):
sv::pathtrace_routine::execute(cmd, pt_trace_desc)   // builds the TLAS + dispatches the GI integrator into the UAV target (no-op if the shaders did not compile)
sv::pathtrace_routine::is_ready(cmd)                 // -> whether the LAST execute dispatched; false before the first one
sv::pt_trace_desc                                    // the trace's targets and constants, plus:
                                                     //   instance_table — one sv::instance_gpu per TLAS instance, in that order
                                                     //   hit_groups     — the permutations, in hit-group index order; tlas_instance::hit_group_offset indexes it
                                                     //   bindless       — &resources.freeze()'s value, bound as the pipeline's second group
sv::pt_frame_constants_gpu                           // { camera_gpu camera; area_light_gpu light; i32 samples_per_pixel, max_bounces; u32 seed, accum_frame; } — 256 bytes

// Also present, driven directly (not by the view_renderer): the flat single-bounce IBL trace.
sv::pbr_raytrace_routine::execute(cmd, trace_desc)   // builds the frame TLAS + one image-based-lit sample per pixel (SH diffuse irradiance + Fresnel env reflection) into the UAV target (no-op if the shaders did not compile)

sv::shader_package()                                 // register once on an slib::shader_library before rendering
```

[`pathtrace_routine.hh`](src/shaped-viewer/rendering/pathtrace_routine.hh) describes the integrator: next-event estimation toward both the area light and the SH environment.
Each is balance-heuristic weighted against the BSDF-sampled bounce ray, so a near-smooth surface under a small light converges instead of sparkling.
That is why it converges at far fewer `samples_per_pixel` than a naive path tracer.
What a caller supplies is a view.
The `view_renderer` builds `pt_frame_constants_gpu` from the view's first `area_light` plus `render_settings::samples_per_pixel` / `max_bounces`.
A view with an empty `area_lights` list falls back to an overhead rect facing down, so the scene is lit even without matching emissive geometry.
That is unlike a Cornell box, whose light rect must match the emitter.
The view's `background` (RGB SH) is packed to `background_gpu` and bound at b1.
The flat and path-tracer misses both reconstruct from it the environment radiance an escaped ray sees; the shadow miss carries visibility only.

**Temporal accumulation** rides on that persistent target: `accum_frame == 0` overwrites, anything above blends into it in place at a weight of `1 / (accum_frame + 1)`.
Nothing caps it — the target is `rgba32_float` and the mean is exact, so a view left alone converges to ground truth rather than settling near it.
Restarting is therefore the whole policy, and the signal is a hash of the bytes the trace uploads — camera, size, lights, background, settings, every instance transform, geometry identity.
Hashing what is uploaded rather than what the view holds is what keeps it from drifting away from the shader.
The camera is in it deliberately: every sample the target holds was drawn through one eye, which is what makes the mean the image this frame is asking for.
`view::position` is deliberately outside it: it only decides where `viewer_renderer` blits, so relayout must not discard a converged image.

## Persistent per-view state

Everything a view keeps across frames hangs off its `view_id`, in **one** store the frame owns — camera, controller, placement, zoom, composite target and accumulators together.

```cpp
sv::view_store                       // owned by sv::viewer, or by whoever drives view_renderer directly; NOT thread-safe
store.begin_frame(u64(ctx.current_epoch()))   // reclaim against the just-finished frame, then advance
store.get_or_create(id) / get / get_ptr       // get asserts the id exists; get_ptr is null when absent; both mark it used
store.peek(id) / peek_ptr(id)                 // the same pair without touching, so a hit-test cannot keep a view alive
store.set_payload_bytes(id, n)                // what the byte budget counts; view_renderer::resolve stamps it
store.accumulated_frames(id) -> u32           // one named slot, defaulting to accumulation(0) — the layer, not the view
store.min_accumulated_frames(id) -> u32       // the lowest across every traced layer; sv::impl::min_accumulated_frames is the rule
store.is_accumulation_converged(id, frames = {}) -> bool   // every traced layer reached `frames`, or stopped at sv::accumulation_frame_cap
sv::impl::view_state                 // the record: display_name, controller, camera, placement, zoom, composite, temporal, last_refresh_frame
sv::impl::temporal_slot              // { texture_2d texture; u64 reset_hash; u32 accum_frame; }
st.temporal                          // cc::map<u64, temporal_slot> — KEYED by temporal_input::id, not indexed by layer

sv::impl::keyed_cache<Key, Record>   // what it is built on — lru_pool's counterpart for NAMED state
c.begin_frame(tick, on_release)      // on_release frees the payload AND clears it; the cache never overwrites the record
sv::impl::keyed_cache_limits         // { i64 max_idle_frames_payload, max_idle_frames_entry; isize max_payload_bytes, max_entries; }
```

The two thresholds are the point: a view's textures are megabytes and go after ~60 idle frames, while the identity behind them costs a few dozen bytes and survives ~240.
That only works because the release hook clears the textures alone — a demotion that reset the record would take the camera with it.

`sg::render_routine::evict` no longer reaches an accumulated image, since none live on a routine.
What restarts a view after a shader reload is the reload generation `view_renderer` folds into its trace hash.

## Viewer + authoring API

`sv::interactive` opens a viewer and hands back its frame loop, which **owns** it — so nothing needs a variable for the viewer and nothing needs tearing down.
No context is threaded through: one is acquired through the provider `sv::set_acquire_context` installs, or from a built-in default.

A frame carries the whole *window* surface, which carries the whole *view* surface, so the shorthand and the long form are the same call:
`f.add_scene()` is exactly `f.window().view().add_scene()`.

```cpp
sv::set_acquire_context(p)       // p = sv::context_provider = cc::unique_function<cc::result<sg::context_handle>()>; unset by default
                                 //   sv::set_acquire_context([] { return sg::create_dx12_context({.use_warp = true}); }); pass {} to clear
                                 //   called AT MOST ONCE per process: the handle it returns is what every viewer gets, so it needs no static of its own
sv::acquire_viewer_context()     // -> cc::result<sg::context_handle>; the provider, or the default, memoized

sv::interactive("id", cfg)       // -> frame_range owning its viewer; cfg = viewer_config { title, width, height, buffer_count, headless }
                                 //   title is optional: unset takes the id, up to its ## — so naming a viewer usually titles it too
                                 //   headless: no window system, no window, no swapchain, nothing presented — composites into an offscreen texture
                                 //   SC_CAPTURE turns this on by itself and installs a capture, but only for an example its .capture.json declares
sv::interactive(ctx, "id", cfg)  // the same on a context the caller owns and keeps alive
sv::viewer::try_create("id", cfg) / ::create("id", cfg)        // the viewer by hand; also the (ctx, ...) overloads
viewer.frames() -> frame_range;  viewer.request_close()
                                 //   yields sv::frame_scope: a frame whose destructor presents, so the loop body needs no present call
                                 //   leaving the loop (window closed, or a `break`) closes the window — the range owns the loop's life

// the manual loop, for an application whose own loop must stay in charge — same frame, same authoring calls
viewer.is_running()             // -> bool; not close-requested, not quit, not device-lost
auto& f = viewer.begin_frame()  // -> frame&, owned by the viewer; `!f.is_open()` means it cannot draw right now, so `continue`
viewer.end_frame()              // presents it — a bare frame ends nothing itself, so every open one needs this

// on a frame — plus everything a window and a view offer, inherited
frame.window() / frame.window("name")  -> window_ref           // the default window, created on first use
frame.view()                           -> view_ref             // == window().view(): the root view the window presents
frame.viewport_size() -> vec2i;  frame.id() -> u64
frame.seconds() / frame.delta_seconds() -> double              // since the loop started / since the previous drawn frame (0 on the first)
                                                               //   both sampled once per frame, so every view animates off the same instant
auto id = frame.scoped_id(i);  frame.id_seed()                 // RAII id scope, so one name in a loop names N views
frame.push_id(i) / frame.pop_id()                              // the same, explicit — every push needs its pop
frame.present()                                                // flatten + record + present; idempotent
                                                               //   a frame_scope's destructor is this call, and viewer::end_frame is too
frame.pending_resource_work() -> isize                         // resources still owing post-load work (mip generation and its kin)
                                                               //   0 does NOT mean settled, and it never restarts accumulation: that work
                                                               //   changes a texture's contents, not its id, so accumulated_frames cannot see it
view.accumulated_frames() -> u32                                // the SLOWEST traced layer's count; 0 for a view with none
                                                               //   never layer 0 by fiat: a layout or ui layer below the scene puts the trace at 1
view.is_accumulation_converged(frames = {}) -> bool             // ASK THIS, not the counter: a layer stops at sv::accumulation_frame_cap (4096),
                                                               //   so a target above the cap is one `accumulated_frames()` can never reach
                                                               //   unset frames = "finished as far as it can"; false for a view with no traced layer
frame.register_capture("name", body)                           // a named capture; body runs INLINE here, every frame, only when it is the one taken
                                                               //   takes sv::capture_context { first_frame, name, size }
                                                               //   MUST be idempotent after the first frame, or accumulation never settles
                                                               //   the name must also appear in the example's .capture.json; a mismatch fails, never falls back

// on a view — the layout layer is created lazily, so you only pay for what you name
view.add_scene()                 -> scene_ref                  // APPENDS a 3D layer; a traced layer is forced to `replace`, so a second one overwrites the first
view.layout_rows(style) / .layout_columns(style) / .layout_grid(c, r, style) / .layout_grid(params, style) / .layout_auto_grid(style, params) -> layout_ref
                                                               //   fills the view with a tree; asking twice returns the SAME tree
                                                               //   all one container: rows pins one column, columns one row, the params form pins either
view.camera(cam)                                               // the caller owns it THIS frame: the controller leaves this view alone
view.initial_camera(cam) / .initial_orbit(orbit)               // applied only the first time this id is seen
view.resolution(vec2i)                                         // pin a fixed pixel size instead of taking the rect it lands in
view.refresh_rate(rate)                                        // 1 every frame, 0.5 every second frame
view.display_name("name") / .display_name() -> string_view     // persistent; defaults to the id up to its ##, and "" restores that default
view.id() -> view_id

// on a layout — the handle IS the container, so nothing has to be closed and there is no "current" one
layout.add_view("id")            -> view_ref                   // a leaf holding one view; the common case
layout.leaf()                    -> leaf_ref                   // an empty leaf, for several views or a post-process
layout.rows(style) / .columns(style) / .grid(c, r, style) / .grid(params, style) / .auto_grid(style, params) -> layout_ref
layout.relative(placement, style)-> layout_ref                 // out of flow, drawn in front — an inset or an overlay
layout.style(box_style)

// on a leaf / a scene
leaf.add_view("id") -> view_ref;  leaf.post_process(p);  leaf.fit(m);  leaf.sampler(m);  leaf.allow_zoom(b)
scene.add_mesh(sv::mesh)         -> mesh_ref                   // geometry + per-face materials upload here, keyed by the mesh's own hashes
scene.add_light(area_light)      -> light_ref                  // both hand back a typed handle rather than chaining
scene.background(bg) / .settings(render_settings)
mesh_ref.transform(t);  light_ref.light(l)
```

Every id — `add_view`, `window`, `push_id`, `scoped_id`, `display_name` — is formattable (`add_view("angle##{}", i)`) and understands ImGui's `##`:
the **whole** string is hashed, so `angle##0` and `angle##1` are two views, while what a human reads is `angle`.

A view's camera, its zoom and its accumulated image are keyed by its `view_id`, so they survive the frame that authored them.
The id stack is deliberately **not** seeded by layout nesting: moving a view between containers must not change its id and throw away what it kept.

### Input

The viewer routes mouse input to the leaf under the cursor, picked in **painter's order** through the plan's `hit_region` parent links.
So a nested view wins over the wrapper containing it, and a shallow overlay still wins over a deep view it covers.
A view whose camera the caller set last frame is skipped, so the two never fight.

- left-drag orbits, middle-drag pans, wheel zooms the **camera**
- **Ctrl+wheel** magnifies the **image** at the cursor instead: it narrows what the leaf samples, never touches a
  camera, and never reaches a trace — so a converged image can be inspected without restarting it.
  Pair it with `sampler_mode::nearest` for a pixel-exact readout.

```cpp
// the whole loop
auto const mesh = sv::mesh{.geometry = sv::triangle_geometry::create_from_positions(positions),  // once: this pins and hashes
                           .attributes = {sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, colors)},
                           .material = sv::default_material(*sv::acquire_material_library().value())};

for (auto f : sv::interactive("main"))
{
    auto rows = f.window().view().layout_rows({.padding = 8, .spacing = 6});

    auto s = rows.add_view("left").add_scene();
    s.add_light({.center = {0, 3, 0}, .half_extent_u = {0.75f, 0, 0}, .half_extent_v = {0, 0, 0.75f}, .emission = {12, 12, 12}});
    s.add_mesh(mesh);   // the upload happens here, and only when a hash changed

    // a view whose layer is another layout — its own texture, subdivided again
    auto inner = rows.add_view("right").layout_columns({.border = 2, .border_color = {0.9f, 0.3f, 0.1f, 1}});
    inner.add_view("a");
    inner.add_view("b");
}

// or, with no ceremony at all: the default window's default view's 3D scene
for (auto f : sv::interactive("simple"))
    f.add_scene().add_mesh(mesh);
```

## Driving the renderer by hand

`sv::interactive` does all of this for you; this is the path for embedding sv in a loop you already own.

```cpp
// setup (once): build the scene through the managers
auto resources = sv::gpu_resource_manager::create(ctx, {.meshes = {.budget = {.max_bytes = 256 << 20}}});
auto const item = resources.acquire_scene_item(mesh);   // geometry + BLAS, the material resolved, its permutation compiled

// per frame: describe -> flatten -> record -> present, in one command list
auto def = sv::viewer_definition{};

auto v = sv::view_data{};
v.id = sv::view_id::from_string("main");
sv::ensure_scene_3d(v).items.push_back(item);
def.views.push_back(cc::move(v));

// a root view whose one layer is a layout naming that view — what sv::viewer synthesizes per frame
auto const root_node = def.nodes.add_container(sv::invalid_node);
auto leaf = sv::layout_leaf{};
leaf.views.push_back(0);
def.nodes.add_leaf(root_node, cc::move(leaf));

auto root = sv::view_data{};
root.id = sv::view_id::from_string("root");
root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
def.root_view = sv::view_index(def.views.size());
def.views.push_back(cc::move(root));

auto const plan = sv::build_render_plan(def, {w, h}, frame_index, history);

auto rt = sc->acquire_backbuffer();
auto cmd = ctx.create_command_list();
resources.advance_to(ctx.current_epoch());                 // both once per frame, before anything reaches for a texture
store.begin_frame(u64(ctx.current_epoch()));
sv::viewer_renderer::execute(*cmd, def, plan, resources, store, rt.cleared(clear_color));
ctx.submit_command_list_and_present(*sc, cc::move(cmd));

// a GUI over the frame is a SECOND pass — every trace must precede any pass, so the frame's cannot be shared
auto scope = cmd->raster.render_to({.color_targets = {rt.preserved()}});   // preserved() keeps the frame underneath
sr::imgui_routine::execute(scope, ImGui::GetDrawData());
```

## Rendering internals

```cpp
sv::view_renderer::resolve(cmd, plan, store) -> plan_resources    // allocates/resizes every texture the plan names, and TOUCHES EVERY REACHABLE VIEW
                                                           //   even a throttled one, or the idle reclaim frees a texture its parent is about to sample
                                                           //   plan.temporals is what it allocates from; a trace's output is looked up by temporal id
sv::plan_temporal                // { view_id id; u64 temporal_id; vec2i resolution; pixel_format format; u64 reset_hash; } — a temporal_input, sized
sv::view_renderer::trace(cmd, def, plan, i, res, resources, store)  // one dispatch, with no rendering scope open
sv::plan_resources               // { vector<texture_2d> targets, traces; } — .textures() -> plan_textures spans; the output's slot is empty (it is the caller's)
sv::viewer_renderer::execute(cmd, def, plan, resources, store, output)   // traces first, then one layout_routine pass per refreshing target
sv::layout_routine::execute(scope, window_id, draws, textures)    // borders + placed views + wipes; the ONLY thing that writes a target
```

`sr::blit_routine` is no longer used by sv: the layout routine owns compositing end to end, so blending, fitting and sampling live in one place.

## Gotchas

- **Ray tracing shaders compile at SM 6.8 through slib** — the payload struct needs the DXR 1.1 annotation
  (`struct [raypayload] Payload { ... : read(...) : write(...); }`), unlike SM 6.3 examples elsewhere.
- **The payload-access qualifiers are checked with `-Werror`** — a `read(caller)` field must actually be read
  after the `TraceRay` (and be written on every path that can run, else "undefined"). Read all payload fields
  into locals right after the trace, and give shadow rays their own minimal payload + miss shader (a shadow
  ray reads only visibility) rather than reusing the surface payload.
- **The frame-constants cbuffer is padded to 256 bytes** — a D3D12 CBV is sized in 256-byte multiples, so a
  smaller backing buffer overruns.
- **A too-small budget thrashes** — a resource whose id a live scene still names must stay resident; if the
  byte budget can't hold a frame's working set, `get_ptr` returns null and the renderer asserts.
- **Indexed and non-indexed are separate paths end to end** — nothing is de-indexed and no index buffer is synthesized.
  `mesh_record::is_indexed` says which a record is, and it reaches the path tracer's closest-hit through `instance_gpu::is_indexed`, per instance.
  The flat `pbr_raytrace_routine` still carries it per frame, in `frame_constants_gpu::mesh_is_indexed` — an `sr::gpu_boolean`, so the plain `bool` off the record assigns straight into it.
  A test driving that routine directly must set it, or it will read `Indices` as if it were real.
  A non-indexed record binds the manager's stand-in there, which no shader reads.
- **Calling `view.camera(...)` every frame restarts the accumulation every frame** — by design, since an animated view has no history worth blending.
  Seed with `initial_camera` / `initial_orbit` instead for a view that should converge.
- **Nothing about placement may reach a trace.** A `view_data` has no position at all, and a leaf's fit, sampler, zoom and post-process
  parameters never reach an upload — which is why relayout, magnifying, or dragging a wipe slider all leave a converged image alone.
  The one deliberate exception is the resolution, folded into the trace hash explicitly: it reaches the upload only through the camera's
  aspect ratio, so 960x540 and 1920x1080 would otherwise hash identically.
- **Two views may not share a `view_id` within one frame** — they would fight over one camera and one accumulator, so it asserts.
  Wrap the body in `frame::scoped_id(i)`, suffix the id with `##i`, or give them distinct names.
- **A layout does not seed the id stack**, on purpose: relayout must not change a view's id and discard its camera and its converged image.
- **`view.layout_rows()` twice returns the same tree** rather than stacking two layouts — a view holds one layout layer.
  `view.add_scene()` is the opposite: it appends every time, so calling it twice in a frame gives a view two traced layers, of which only the last is visible.
- **A dispatch may not be recorded inside a rendering scope.** dx12 tolerates it, Vulkan does not, and sg now asserts;
  it is why the plan's traces all hoist above every pass.
- **`sg::render_routine::evict(ctx)` no longer restarts anything** — the accumulated images live in the caller's `view_store`, which outlives the routine instance.
  A shader reload still restarts every view, but through the reload generation folded into the trace hash rather than through eviction.
- **The texture `view_renderer::execute` returns is not yours** — it is keyed by `view.id` and may be resized or released by the next `execute` / `store.begin_frame` for that id.
