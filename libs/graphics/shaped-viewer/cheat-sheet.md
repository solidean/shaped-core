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
sv::temporal_inputs_of(view_data) -> vector<temporal_input>  // what the view declared, PLUS one accumulator per scene_3d layer
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
sv::scene_item                   // { scene_item_kind kind; mesh_id mesh; material_set_id materials; tg::affine_transform3f transform; } — triangle_mesh only for now
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
sv::pbr_material                 // { vec3f base_color, emissive; float metallic, roughness; } — flat, per-triangle
sv::pbr_attribute::base_color / ::metallic / ::roughness / ::emissive   // the per_triangle attribute names per-face PBR travels under
sv::pbr_material_attributes(materials) -> vector<mesh_attribute>        // scalarizes an AoS range into those four; TEMPORARY, until an attribute can hold a struct
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
sv::mesh_texture                 // { string name; texture_id texture; } — a texture offered under a slot name the material binds
sv::material_id                  // thin handle naming ONE material definition (owned elsewhere); nothing mints one yet
```

The material is what gives the three lists their meaning: it decides which attribute names it samples, which texture slots it binds, and how the flags change what it emits.
Per-mesh values are attributes too, at `per_instance` — one list, so a renderer can pack every instance's values into one buffer and index it by instance.
So a mesh may carry data no material uses and miss data another would want — a material falls back rather than failing.
The mesh offers no by-name lookup, deliberately: a material resolves the names it wants once, into whatever binding table it draws from, rather than scanning strings per draw.
Copying a mesh shares the pinned payloads (a refcount bump), so passing one around is cheap.
Nothing renders an `sv::mesh` yet: the renderer still consumes `sv::scene_item` (ids into the managers), and the bridge is `triangle_data::from(triangle_geometry)`.
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
sv::scene_resources::create(ctx, cfg)  // named ctor; cfg = { manager_config meshes, materials; bindless_config bindless }
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
material_manager::acquire(attributes, triangle_count) -> material_set_id
                                                             //   the same from a mesh's per_triangle sv::pbr_attribute lists — what scene_ref::add_mesh calls
                                                             //   keyed by folding the attributes' own hashes, so it never re-hashes their bytes; a missing one falls back to pbr_material's default
manager.get(id) / get_ptr(id) / contains(id)        // resolve an id back to its record (get_ptr also LRU-touches)
manager.set_limits(max_bytes, max_idle_epochs)      // change the budget at runtime (0/‑1 = unbounded/never)
manager.used_bytes() / count() / evict(id)          // current residency; manual drop
scene_resources.begin_frame(epoch)                  // reclaim + advance; the CALLER owns this — once per frame, no routine does it
sv::mesh_id / material_set_id / tlas_id / texture_id / buffer_id   // enum class : u32; ::invalid == u32(-1) (ids mint from 0)
```

The managers ride on `sv::impl::lru_pool<Id, Record>`, the reusable id-pool.
It mints ids, tracks each record's byte size and last-used epoch, and evicts on the idle timeout or the byte budget, least-recently-used first.
It never evicts this frame's working set; `begin_frame` in its header states that rule exactly.
It is content-addressed: records go in under the caller-supplied `cc::hash128`, so `acquire` is O(1) and never re-uploads content it already holds.
A manager never hashes anything itself, so hash load stays where the caller schedules it and never lands inside a per-frame acquire.

## The bindless group — sv::bindless_manager

```cpp
sv::bindless_manager::create(ctx, cfg)  // named ctor; cfg = bindless_config { u32 buffer_count, texture_{1d,2d,3d,cube}_count (each >= 2);
                                        //   cc::string {buffers,textures_{1d,2d,3d,cube}}_binding }  — capacities AND binding names are config
manager.config()                        // -> bindless_config const& — read the names back for declare_array_*_access
manager.acquire(readonly_buffer_view<byte>)  // -> bindless_buffer_slot      compile-time readonly; same view -> same slot, O(1), no reupload
manager.acquire(readonly_texture_view<sg::tv_1d / tv_2d / tv_3d / tv_cube>)  // -> the category's slot newtype
manager.layout()                        // -> binding_group_layout_handle — one slot of the consumer's pipeline layout (lazy)
manager.lock()                          // -> binding_group_handle — the staging group's snapshot: minted ONLY if a descriptor changed, else the SAME handle; locks (no acquires)
manager.unlock(group)                   // must get the served group back (pointer identity), in the SAME epoch — both asserted
manager.lock_scoped()                   // -> sv::bindless_lock — RAII form: carries .group(), unlocks at scope exit; move-only
sv::bindless_buffer_slot / bindless_texture_{1d,2d,3d,cube}_slot  // enum class : u32; ::invalid; u32(slot) is what a shader consumes
```

One readonly `sg::binding_group` of five bounded arrays, one register space per category (`space1..space5`, index 0).
Slots are valid ONLY for the epoch they were acquired in — re-acquire the working set every epoch; a full table clears EVERY slot not acquired this epoch (the mint recreates the group anyway).
The descriptors live in one sg::staging_binding_group; `impl::slot_table` per category maps view-identity keys to element indices, and an unchanged epoch serves the cached snapshot untouched.
Access declaration is the CONSUMER's job — declare the elements a dispatch reads via `cmd.*.declare_array_*_access` with the binding names above.
Writable views are never bindless; they stay ordinary bindings in another group.

## Rendering — the view_renderer + routines

```cpp
// Both the FRAME's job — once, before the first view resolves its ids or reaches for its accumulator.
resources.begin_frame(ctx.current_epoch())
store.begin_frame(u64(ctx.current_epoch()))               // reclaims idle view textures; skipping it only means nothing is reclaimed

// One view -> the texture the store keeps under its id. A convenience for a caller tracing a single view with no plan.
sv::view_renderer::execute(cmd, view_data, resources, store) -> sg::texture_2d
                                                          //   traces into a PERSISTENT rgba16f target keyed by v.id, sized from v.resolution
                                                          //   blends in place, restarting whenever the traced image changed at all
                                                          //   the store owns it: holding it past the next execute/begin_frame for that id is invalid
                                                          //   assumes ONE traced layer per view; the plan path below is per (view, layer)
store.accumulated_frames(view_id) -> u32                  // frames accumulated so far; 0 after a restart. For tests and debug overlays

// The whole frame, from its plan: every trace first, then one pass per refreshing target, the output last.
sv::viewer_renderer::execute(cmd, def, plan, resources, store, output)   // output = a sg::color_target, e.g. rt.cleared(clear_color)
                                                          //   `store` MUST be the one `plan`'s view_history was read from
                                                          //   nothing writes the gaps a layout leaves — pass output.cleared(...)
                                                          //   an empty plan still opens no pass; an empty def leaves the clear alone to land

// The leaf routines they drive — each an sg::render_routine<> (everything that traces/draws is a routine):
sv::pathtrace_routine::execute(cmd, pt_trace_desc)   // builds the TLAS + dispatches the GI integrator into the UAV target (no-op if the shaders did not compile)
sv::pt_frame_constants_gpu                           // { camera_gpu camera; area_light_gpu light; i32 samples_per_pixel, max_bounces; u32 seed, accum_frame; } — 256 bytes

// Also present, driven directly (not by the view_renderer): the flat single-bounce IBL trace.
sv::pbr_raytrace_routine::execute(cmd, trace_desc)   // builds the frame TLAS + one image-based-lit sample per pixel (SH diffuse irradiance + Fresnel env reflection) into the UAV target (no-op if the shaders did not compile)

sv::shader_package()                                 // register once on an slib::shader_library before rendering
```

[`pathtrace_routine.hh`](src/shaped-viewer/rendering/pathtrace_routine.hh) describes the integrator: next-event estimation toward both the area light and the SH environment.
That is why it converges at far fewer `samples_per_pixel` than a naive path tracer.
What a caller supplies is a view.
The `view_renderer` builds `pt_frame_constants_gpu` from the view's first `area_light` plus `render_settings::samples_per_pixel` / `max_bounces`.
A view with an empty `area_lights` list falls back to an overhead rect facing down, so the scene is lit even without matching emissive geometry.
That is unlike a Cornell box, whose light rect must match the emitter.
The view's `background` (RGB SH) is packed to `background_gpu` and bound at b1.
The flat and path-tracer misses both reconstruct from it the environment radiance an escaped ray sees; the shadow miss carries visibility only.

**Temporal accumulation** rides on that persistent target: `accum_frame == 0` overwrites, anything above blends in place, capped at 4096 frames so the running mean stays inside half-float precision.
The restart signal is a hash of the bytes the trace uploads — camera, size, lights, background, settings, every instance transform, geometry identity.
Hashing what is uploaded rather than what the view holds is what keeps it from drifting away from the shader.
`view::position` is deliberately outside it: it only decides where `viewer_renderer` blits, so relayout must not discard a converged image.

## Persistent per-view state

Everything a view keeps across frames hangs off its `view_id`, in **one** store the frame owns — camera, controller, placement, zoom, composite target and accumulators together.

```cpp
sv::view_store                       // owned by sv::viewer, or by whoever drives view_renderer directly; NOT thread-safe
store.begin_frame(u64(ctx.current_epoch()))   // reclaim against the just-finished frame, then advance
store.get_or_create(id) / get / get_ptr       // get asserts the id exists; get_ptr is null when absent; both mark it used
store.peek(id) / peek_ptr(id)                 // the same pair without touching, so a hit-test cannot keep a view alive
store.set_payload_bytes(id, n)                // what the byte budget counts; view_renderer::resolve stamps it
store.accumulated_frames(id) -> u32
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

sv::interactive("id", cfg)       // -> frame_range owning its viewer; cfg = viewer_config { title, width, height, buffer_count }
                                 //   title is optional: unset takes the id, up to its ## — so naming a viewer usually titles it too
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
                           .attributes = sv::pbr_material_attributes(materials)};                // per-face PBR, scalarized

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
auto resources = sv::scene_resources::create(ctx, {.meshes = {.budget = {.max_bytes = 256 << 20}}});
auto mesh = resources.meshes.acquire(sv::triangle_data::create(positions));   // or indexed_triangle_data::create(positions, indices)
auto mats = resources.materials.acquire(sv::material_data::create(materials));

// per frame: describe -> flatten -> record -> present, in one command list
auto def = sv::viewer_definition{};

auto v = sv::view_data{};
v.id = sv::view_id::from_string("main");
sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = mats});
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
resources.begin_frame(ctx.current_epoch());                 // both once per frame, before anything reaches for a texture
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
- **One mesh per view for now** — the trace binds the first item's vertex/material buffers; multi-mesh needs
  per-instance indexing (a flagged extension). The TLAS is still built from every item.
- **A too-small budget thrashes** — a resource whose id a live scene still names must stay resident; if the
  byte budget can't hold a frame's working set, `get_ptr` returns null and the renderer asserts.
- **Indexed and non-indexed are separate paths end to end** — nothing is de-indexed and no index buffer is synthesized.
  `mesh_record::is_indexed` says which a record is, and it must reach the closest-hit through `frame_constants_gpu::mesh_is_indexed` / `pt_frame_constants_gpu::mesh_is_indexed`.
  That field is an `sr::gpu_boolean` (shaped-rendering owns it), so the plain `bool` off the record assigns straight into it.
  The `view_renderer` sets it for you; a test driving a routine directly must set it, or the flat path will read `Indices` as if it were real.
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
