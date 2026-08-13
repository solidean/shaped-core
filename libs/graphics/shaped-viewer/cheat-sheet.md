# shaped-viewer cheat sheet

Professional, RTX-enabled visualization renderer.
Namespace `sv`.
Depends on shaped-rendering.
Headers are included by full path from `src/`: `#include <shaped-viewer/<name>.hh>`.

> **Scope note:** first vertical slice — a single path-traced view, blitted into a window by the `view_renderer` routine.
> Rendering needs a ray-tracing backend: dx12 + DXR on Windows today, since vulkan RT is stubbed upstream.
> The API is present everywhere; without a backend a routine just draws nothing.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-viewer/all.hh>   // umbrella
```

## Per-frame description — what to render

```cpp
sv::viewer_definition            // { cc::vector<sv::view> views; } — a whole frame's worth of views
sv::view                         // { view_id id; vec2i size; camera; vector<scene_item> items; vector<area_light> area_lights; background; render_settings; }
sv::view_id                      // stable identity across frames; view_id::from_string("main") — keys persistent resources
sv::camera                       // { pos3d position; quat_d orientation; perspective_projection projection; } — double-precision pose + projection
sv::camera::looking_at(eye, target, up=+y)     // -> camera at eye aimed at target (static factory); default projection
sv::camera::orbiting(target, distance, azimuth, elevation)  // -> camera orbiting target, looking inward (azimuth around +y, elevation off the horizon)
sv::camera::look_rotation(eye, target, up=+y)  // -> quat_d aiming from eye at target (static); cam.look_at(target, up=+y) sets it from position
sv::perspective_projection       // { angle_d vertical_fov; f64 aspect_ratio; f64 near_plane; } — the only projection kind for now
sv::camera_gpu::from(cam)        // -> camera_gpu (the GPU basis: forward/right_scaled/up_scaled); aspect comes from projection.aspect_ratio
sv::render_settings              // { int samples_per_pixel, max_bounces; } — view-wide integration controls (no light/sky: those are on the view)
sv::scene_item                   // { scene_item_kind kind; mesh_id mesh; material_set_id materials; mat4f transform; } — triangle_mesh only for now
sv::area_light                   // { pos3f center; vec3f half_extent_u, half_extent_v; vec3f emission; } — a world-space rect emitting along cross(half_extent_u, half_extent_v); one typed list per light kind on the view
                                 //   emission has no default (it is -1): set it, or the first use warns to stderr
sv::area_light_gpu::from(light)  // -> area_light_gpu { vec3f center, u, v, emission, normal; } — the rect in GPU lane layout (u/v are the half-extents, normal = cross(u, v))
sv::background                   // { vec3f sh[16]; } — order-3 RGB SH environment a missed ray sees (the flat and pt misses both reconstruct radiance from it)
sv::background_gpu::from(bg)     // -> background_gpu { vec4f sh[16]; } — GPU lane layout (each coeff widened to a vec4); the miss's Background cbuffer at b1
sv::pbr_material                 // { vec3f base_color, emissive; float metallic, roughness; } — flat, per-triangle
```

## Mesh authoring — geometry + what a material reads

One header per part — `mesh.hh` pulls in `triangle_geometry.hh`, `mesh_attribute.hh`, `mesh_flags.hh`, `mesh_parameter.hh` and `mesh_texture.hh`.

```cpp
sv::mesh                         // { string name; triangle_geometry geometry; vector<mesh_attribute> attributes; affine_transform3f transform;
                                 //   material_id material; mesh_flags flags; vector<mesh_parameter> parameters; vector<mesh_texture> textures; }
m.is_visible()                   // -> bool (flags.has(mesh_flag::visible)); the rest of a mesh is plain public data
sv::triangle_geometry            // { pinned_data<pos3f const> positions; pinned_data<u32 const> indices; hash128 hash; } — raw or indexed, one type
sv::triangle_geometry::create_from_triangles(triangles)            // -> from a range of tg::triangle3f; the pin is reinterpreted onto the positions, never copied
sv::triangle_geometry::create_from_indexed_triangles(pos, indices) // -> 3 indices per triangle, each < positions.size()
g.is_indexed() / g.is_empty() / g.vertex_count() / g.triangle_count()  // triangle_count follows the index buffer when there is one
sv::triangle_data::from(g) / sv::indexed_triangle_data::from(g)    // -> the mesh_manager payload, same hash, sharing the pin (asserts on the wrong layout)
sv::mesh_attribute               // { string name; attribute_format format; attribute_frequency frequency; pinned_data<byte const> data; hash128 hash; }
sv::mesh_attribute::create(name, frequency, elements)       // -> mesh_attribute; format deduced from the element type, bytes pinned + hashed
a.element_count() / a.elements_as<tg::vec3f>()              // -> isize / span<T const> (asserts if T is not what format names)
sv::scalar_type                  // i8 i16 i32 i64 | u8 u16 u32 u64 | f32 f64 | boolean (1 byte, 0/1) — the complete set; scalar_type_size(t) -> i32 bytes
sv::attribute_format             // { scalar_type scalar; int dim0, dim1; } — scalar + dimensionality, so every scalar/shape combination exists
                                 //   dim0 alone is a vector's component count; for a matrix dim0 is its rows and dim1 its columns
attribute_format::of_scalar(s) / of_vector(s, dim) / of_matrix(s, rows, cols)  // both dims in 1..4; of_matrix is column-major, matching tg::mat<C, R, T>
f.size_bytes() / f.component_count() / f.is_scalar() / f.is_vector() / f.is_matrix()
sv::attribute_format_of<T>       // the format of an element type — scalars and tg vec / pos / comp / mat over them
sv::attribute_frequency          // per_vertex | per_corner (3 per triangle, in triangle order) | per_triangle | per_edge (RESERVED, create asserts)
sv::mesh_flag / sv::mesh_flags   // visible | casts_shadow | receives_shadow (cc::flags); mesh_flags_default is all three — the EMPTY set draws nothing
sv::mesh_parameter               // { string name; parameter_value value; } — per-mesh (instance) values the material reads by name
sv::parameter_value              // { attribute_format format; byte storage[max_bytes]; } — typed by the SAME format an attribute's elements are
                                 //   parameter_value::max_bytes is 32: 4 components of 8 bytes, so every scalar and vector fits inline
sv::parameter_value::of(x)       // -> parameter_value from any scalar or vector (matrices don't fit the inline budget and assert)
p.holds<T>() / p.as<T>()         // -> bool / T (as asserts on the wrong type); holds<T>() is what a material asks first
sv::mesh_texture                 // { string name; texture_id texture; } — a texture offered under a slot name the material binds
sv::material_id                  // thin handle naming ONE material definition (owned elsewhere); nothing mints one yet
```

The material is what gives the four lists their meaning: it decides which attribute names it samples, which parameters it reads, which texture slots it binds, and how the flags change what it emits.
So a mesh may carry data no material uses and miss data another would want — a material falls back rather than failing.
The mesh offers no by-name lookup, deliberately: a material resolves the names it wants once, into whatever binding table it draws from, rather than scanning strings per draw.
Copying a mesh shares the pinned payloads (a refcount bump), so passing one around is cheap.
Nothing renders an `sv::mesh` yet: the renderer still consumes `sv::scene_item` (ids into the managers), and the bridge is `triangle_data::from(triangle_geometry)`.
The seeds behind every content key live in `impl/content_hash.hh`, so a geometry and the payload it is uploaded as agree on one key instead of caching the same bytes twice.

## Resources by id — the managers

```cpp
sv::scene_resources::create(ctx, cfg)  // named ctor; cfg = { manager_config meshes, materials } with per-manager budgets
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
manager.get(id) / get_ptr(id) / contains(id)        // resolve an id back to its record (get_ptr also LRU-touches)
manager.set_limits(max_bytes, max_idle_epochs)      // change the budget at runtime (0/‑1 = unbounded/never)
manager.used_bytes() / count() / evict(id)          // current residency; manual drop
scene_resources.begin_frame(epoch)                  // reclaim + advance; view_renderer::execute calls this for you
sv::mesh_id / material_set_id / tlas_id / texture_id / buffer_id   // enum class : u32; ::invalid == u32(-1) (ids mint from 0)
```

The managers ride on `sv::impl::lru_pool<Id, Record>`, the reusable id-pool.
It mints ids, tracks each record's byte size and last-used epoch, and evicts on the idle timeout or the byte budget, least-recently-used first.
It never evicts this frame's working set; `begin_frame` in its header states that rule exactly.
It is content-addressed: records go in under the caller-supplied `cc::hash128`, so `acquire` is O(1) and never re-uploads content it already holds.
A manager never hashes anything itself, so hash load stays where the caller schedules it and never lands inside a per-frame acquire.

## Rendering — the view_renderer + routines

```cpp
// The view_renderer is itself a render routine: it path-traces each view and blits it into the output, on one cmd.
sv::view_renderer::execute(cmd, def, resources, output)   // output = a sg::color_target, e.g. rt.cleared(clear_color)
                                                          //   traces every view (transient rgba16f target) then blits the first into `output`
                                                          //   opens the raster scope itself; caller just submits (+ presents)

// The leaf routines it drives — each an sg::render_routine<> (everything that traces/draws is a routine):
sv::pathtrace_routine::execute(cmd, pt_trace_desc)   // builds the TLAS + dispatches the GI integrator into the UAV target (no-op if the shaders did not compile)
sr::blit_routine::execute(scope, src_texture)        // fullscreen-triangle blit of a texture across an open raster scope (lives in shaped-rendering)
sv::pt_frame_constants_gpu                           // { camera_gpu camera; area_light_gpu light; i32 samples_per_pixel, max_bounces; u32 seed, accum_frame; } — 256 bytes
sv::gpu_boolean                                      // { u32 value; } — a bool as a cbuffer lane: implicit from bool, explicit to bool, false==0/true==1 (shader may declare it bool or uint)

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

## Typical frame

```cpp
// setup (once): build the scene through the managers
auto resources = sv::scene_resources::create(ctx, {.meshes = {.budget = {.max_bytes = 256 << 20}}});
auto mesh = resources.meshes.acquire(sv::triangle_data::create(positions));         // or indexed_triangle_data::create(positions, indices)
auto mats = resources.materials.acquire(sv::material_data::create(materials));

// per frame: describe → render (trace + blit) → present, in one command list
auto def = sv::viewer_definition{};
auto v = sv::view{.id = sv::view_id::from_string("main"), .size = {w, h}};
v.items.push_back({.mesh = mesh, .materials = mats});
def.views.push_back(cc::move(v));

auto rt = sc->acquire_backbuffer();
auto cmd = ctx.create_command_list();
sv::view_renderer::execute(*cmd, def, resources, rt.cleared(clear_color));
ctx.submit_command_list_and_present(*sc, cc::move(cmd));
```

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
  That field is a `gpu_boolean`, so the plain `bool` off the record assigns straight into it.
  The `view_renderer` sets it for you; a test driving a routine directly must set it, or the flat path will read `Indices` as if it were real.
  A non-indexed record binds the manager's stand-in there, which no shader reads.
