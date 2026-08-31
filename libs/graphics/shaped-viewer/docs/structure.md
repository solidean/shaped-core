# shaped-viewer structure (sv::)

The living roadmap for shaped-viewer.
Section headers carry a status tag: **[done]** / **[in progress]** / **[planned]**.
This document is design intent, not a guarantee of final API.

shaped-viewer is built around a **recursive view model**: a view is the definition of one texture, and one of a view's
layers may be a whole layout tree — so views nest, at any depth.
A frame is authored through fluent handles, flattened into a `render_plan`, and replayed by `viewer_renderer`.
Raytracing-first — raster is a later fallback / special case.

## Goals

A professional, RTX-enabled visualization renderer with a dev-friendly API, built on shaped-rendering (`sr::`).
It is the top of the graphics stack and the intended home for Shaped Code's visualization needs — SOLIDEAN, internal tools, customer projects.

## Module layout

The source tree names these groups directly: the authoring surface (`viewer` / `frame` / `refs` / `interactive` / `context`) is what sits at the root of `src/shaped-viewer/`,
and everything else is one folder down — `scene/` is what a caller puts into a view, `view/` is what a view is and how it is framed, then `layout/`, `resources/`, `rendering/` and `impl/`.

```text
view_data / layer / viewer_definition    [done]         a view is ONE TEXTURE: id, resolution, camera, ordered layers, refresh policy, temporal inputs.
                                                        Deliberately no position — the leaf referencing it decides that, which is what keeps relayout from restarting an image
layout tree (layout/)                    [done]         grid / relative / leaf, each with margin, border, padding and spacing.
                                                        One grid kind spans linear, fixed and automatic: an unset `grid_params` dimension is derived from the child count
                                                        A `relative` node is out of flow: siblings tile as if absent and it draws in front (this replaced pinning)
render_plan                              [done]         the recursive frame flattened into recording order: targets in dependency order, traces, per-target draw lists,
                                                        window-space hit regions and diagnostics. A pure value, so every invariant is testable without a GPU
layout_routine                           [done]         the ONE routine that writes a target: border bands, each view placed with its fit / sampler / blend, and the wipe.
                                                        sv no longer uses sr::blit_routine — compositing lives in one place
view_renderer                            [done]         allocates every texture a plan names (touching even throttled views) and records one trace per scene_3d layer
viewer_renderer                          [done]         replays a plan: every trace first, then one layout_routine pass per refreshing target. Nesting falls out of the ordering
headless capture (capture.hh)            [done]         SC_CAPTURE runs the loop with no window system, no window and no swapchain, composites into an offscreen texture,
                                                        waits for the image to settle and writes it. An example costs zero lines; docs/guides/examples.md is the workflow.
                                                        Settled is a conjunction: accumulated frames per traced view, no pending post-load work, AND a trace that dispatched —
                                                        the last is what stops an uncompiled pipeline from writing a black image at full count
authoring API (interactive / refs)       [done]         sv::interactive -> frame -> window_ref -> view_ref -> layout_ref / leaf_ref / scene_ref.
                                                        A frame inherits the window surface, which inherits the view surface, so f.add_scene() == f.window().view().add_scene()
context provider (set_acquire_context)   [done]         an overridable hook, called at most once per process; the default brings up dx12 (hardware, then WARP)
shader library provider                  [done]         the same shape for slib: the library is process-wide rather than a viewer's, because a generated material permutation is compiled from the render path.
                                                        The default registers sv's and sr's packages plus DXC; viewer and the GPU tests both reach it through the hook rather than each assembling one
input routing + key-bound zoom           [done]         picks the leaf under the cursor in painter's order through the plan's region links;
                                                        Ctrl+wheel magnifies what a leaf samples without touching a camera or a trace
mesh / triangle_geometry / attributes    [in progress]  the authoring-side mesh: triangle_geometry (raw or indexed, pinned + hashed) plus named attributes (per element, or per instance for a per-mesh value), a material id, flags and textures
resource managers (mesh / material)      [in progress]  strongly-typed ids -> GPU resources (BLAS built here); LRU budget + idle eviction
resource data (triangle / indexed / material)  [in progress]  what a caller uploads: a pinned_data payload + its cc::hash128 content key
gpu_resource_manager                     [in progress]  where resource management comes together: the four managers, the staging binding group, and one sg::bindless_array per table.
                                                        Its tick is `advance_to(epoch)` — idempotent, so N windows drawing at N rates each call it and the first one pays.
                                                        It holds the bindless lock too, since that invariant spans every array over one group rather than any single one
bindless tables                          [in progress]  sv hand-declares the layout (resources/bindless_tables.hh): one table per view dimension, byte-address buffers, budgets from the config.
                                                        Every generated permutation reads gBindlessBuffers — its epilogue reaches the mesh's positions through it — and declares gBindlessTextures2D only where an attribute samples one; both are bound as the trace's second group
pathtrace_routine                        [in progress]  the DXR GI trace view_renderer drives: TLAS + dispatch_rays into a UAV target
pbr_raytrace_routine                     [in progress]  the flat single-bounce IBL DXR trace (SH environment), driven directly
sv_shaders package                       [in progress]  raygen / miss+closest-hit, plus layout.hlsl (border / view / wipe), via slib
camera / controls                        [in progress]  dev-friendly pinhole camera, plus sv::orbit_camera_controller (event-driven) and sv::fps_camera_controller; only the orbit one is wired in
persistent per-view state                [in progress]  view_id keys what a view keeps — camera, controller, zoom, display name, last rect, composite target and accumulators — all in one sv::view_store the frame owns
id stack (push_id / scoped_id)           [done]         seeds view_id so one name under N scopes names N views; independent of layout nesting, and a duplicate within a frame asserts.
                                                        Ids are formattable and take an ImGui-style ## suffix, which separates two views without changing what a human reads
temporal accumulation                    [in progress]  a traced layer blends into one rgba32_float target in place, uncapped; the camera or the scene changing restarts it, nothing else does
textures + post-load work                [in progress]  texture_manager uploads and pins an element per texture; residency says how much has landed.
                                                        Follow-up steps (mip generation through sr::box_filter_mipmap_routine) are QUEUED and drained under a per-epoch dispatch budget, which is the microstutter guard.
                                                        Still to come: async streaming, placeholders while pending, and mapping visibility onto sg's stream priorities
material system (material/)              [in progress]  material_type (signature + HLSL fragment) -> material (a type with attributes bound) -> material_library (content-addressed, never evicted, provider hook like the context's).
                                                        resolve_material walks one attribute down the frequency chain — type default, material, per-instance, mesh attribute, material texture, mesh texture — finest wins, `final` blocks finer.
                                                        It yields TWO keys: permutation_key over the resolution's shape (what a shader is generated from), parameter_key over its values (what a per-instance slot is filled from).
                                                        So two materials differing only in constants share one shader; only a texture sample forces a second.
                                                        CPU-side and complete
material shader generation               [in progress]  generate_material_shader turns a resolved_material into HLSL plus the parameter layout that source reads.
                                                        Emits only the bindless tables the permutation touches (names and spaces from bindless_tables.hh), one SamplerState per distinct sampler, and one initializer per attribute — a parameter-block load, a barycentric interpolation, or a uv sample.
                                                        The type's fragment then runs verbatim over those locals; shaders/material_runtime.hlsli is the hand-authored half it is written against.
                                                        Compiled through slib::shader_library::compile_source
material data on the GPU                 [in progress]  attribute_manager uploads any mesh_attribute to a byte-address buffer keyed on its own hash, which is what makes the mesh_attribute rank of the chain reachable at all.
                                                        gpu_resource_manager::acquire_instance resolves a parameter block down to ids, content-keyed on parameter_key; build_instance_parameters turns one into bytes for THIS epoch, into the record's own persistent buffer.
                                                        Every index in a block is that epoch's, minted where it is written — which is what makes the trace's access declaration complete by construction rather than by remembering.
                                                        sv::instance_gpu is the per-item record a closest-hit reads by InstanceID() — its material's parameter block, and its own geometry, all four indices acquired by describe_instance.
                                                        Mesh geometry is acquired into the buffers table too, so a view is no longer limited to one mesh by its bindings.
                                                        material_shader_cache — which gpu_resource_manager owns, so a mesh is authored and its shader acquired in one call — compiles one closest-hit per material_shader_key through sv::acquire_shader_library.
                                                        So gold and copper are one compile and only a texture sample costs a second.
                                                        scene_ref::add_mesh resolves the material and the block; view_renderer builds one instance_gpu per item on its own command list, and pathtrace_routine builds a DXR pipeline with one hit group per permutation, selected per instance by tlas_instance::hit_group_offset.
                                                        The trace binds two groups: its own bindings, and the manager's bindless tables.
                                                        Still to come: several parameter blocks per buffer, and a local root signature so two permutations may disagree about a sampler register
glTF material mapping                    [done]         openpbr declares alpha_cutoff (glTF's MASK, 0 = off) and occlusion (imported, and deliberately ignored by the path-tracing fragment — a baked AO term double-counts what the integrator computes per bounce).
                                                        texture_sample_source carries a channel_swizzle, so one packed metallic-roughness or ORM map binds several attributes over a single upload.
                                                        The swizzle is generated code rather than a value, so it lives in permutation_key — and only as far as the declaration reads it, which canonicalizes identity swizzles.
                                                        A sample_transform beside it splits the other way: a per-component scale and bias, of which only the EXISTENCE is shape, so a normal map's decode and its scale never fork a permutation
mesh_data / mesh type split              [done]         a mesh exists in two forms, and both are first-class: sv::mesh_data is pinned CPU bytes and needs no device, sv::mesh is resources and cannot exist without a manager.
                                                        gpu_resource_manager::create_mesh is the one-way bridge, scene_ref::add_mesh takes either, and resolution runs against the GPU form — which is what admits geometry that was never on the CPU.
                                                        Textures are symmetric with geometry now: a mesh_data carries pixels, an sv::mesh a texture_id.
                                                        The GPU form also keeps the CPU-side summary (bounds, triangle and vertex counts) that nothing else could answer a framing question with
deferred uploads + placeholders          [done]         every record carries an sv::residency, and an acquire mints an id and QUEUES the payload rather than uploading it.
                                                        record_pending_work drains at the epoch's byte budget (unbounded by default), attributes before geometry so an attribute is up before the mesh indexing it draws.
                                                        A pending mesh is traced as one shared unit-cube BLAS scaled onto its declared bounds and shaded through the fallback hit group; one with no bounds is skipped.
                                                        The viewer drains once per frame — which nothing did before, so a viewer's textures never grew their mip chains either
fallback hit group                       [done]         a permutation that has not compiled — still in flight, or a material that does not build — is replaced for that trace by a neutral hit group over an empty signature, which reads no per-instance block and so stands in for any material.
                                                        The pipeline is keyed on the substituted set, so the frame the real one lands a new variant is built with it.
                                                        Before this, one bad material made the whole view a no-op
asset loading (asset_loader)             [in progress]  glTF 2.0, OBJ and STL load into a CPU-side sv::asset_data that add_mesh takes directly — one mesh per (geometry, material), hierarchy flattened, material names namespaced by the asset.
                                                        The loader holds no device and opens no file: every uri goes through sv::resolve_uri, a settable process-wide hook over cc::file_read_stream_adapter.
                                                        Textures ride on the mesh as pixels rather than on the material as ids, which is what keeps the whole importer CPU-side.
                                                        Still to come: PLY, .mtl, the KHR_materials_* extensions (babel does not read them), generated tangent frames, and the state / recipe / substitution model in [asset-loading.md](asset-loading.md)
lighting                                 [planned]      a scene layer holds typed light lists + an SH background; more light kinds next
scene_2d layer                           [planned]      typed and validated, draws nothing: shaped-core has no 2D renderer at all, so this needs one built first
ui layer                                 [planned]      Dear ImGui into a view's own target, through sr::imgui_context / sr::imgui_routine
multi-window                             [planned]      the plan and the layout routine are window-aware already; sv::viewer still owns exactly one window
```

## Persistent per-view state

A view is re-submitted every frame as a fresh value; its `view_id` is what ties it to everything it keeps.
All of that lives in **one `sv::view_store`**, whose record is `sv::impl::view_state`.
One record holds both halves.
What a caller or the built-in controller drives is the camera, the orbit, the zoom, the last rect and the hover / drag flags.
What the renderer writes is the view's composited target plus one accumulation slot per traced layer.

**The store belongs to whoever runs the frame**, not to a routine.
`sv::viewer` owns one; a caller driving `view_renderer` directly owns one of their own.
That is what keeps the renderer usable with nothing but a command list, and keeps two viewers on one context out of each other's id space.
It reaches `view_renderer::resolve` / `::trace` / `::execute` and `viewer_renderer::execute` as a parameter.

One store rather than two is the invariant, not a convenience.
An identity cannot outlive its texture, and a texture cannot be released out from under the parent still sampling it, because one reclamation decides both.
`build_render_plan` stays a pure function all the same, taking what the store already holds as a `view_history` value.
That value is read off the composite texture rather than off a field stamped beside it, so what the plan believes it can re-present and what there is to re-present cannot drift apart.

Two tiers of idleness fall out of the size difference: a view's textures are megabytes and are released after ~60 idle frames, while the identity behind them costs a few dozen bytes and survives ~240.
That only works because `keyed_cache`'s release hook clears exactly what it freed and the cache never overwrites the record.
A demotion that reset the whole record would take the camera with the accumulator.

Nothing in the store signals the trace.
The trace decides for itself whether its image is still valid, by hashing the bytes it uploads, so a stale field restarts the image rather than corrupting it.
A shader reload is folded into that same hash as a reload generation, since `sg::render_routine::evict` no longer reaches an image the caller owns.

## Platform / backend status

Ray tracing runs on **dx12 + DXR** (Windows), hardware or WARP.
Vulkan ray tracing is real in shaped-graphics now, and sv still does not run on it — the blocker moved rather than cleared.
Both backends take HLSL, and it is not the same HLSL.
A SPIR-V target needs `[[vk::binding]]`, `[[vk::location]]` and `[[vk::push_constant]]` annotations — see sg's [shaders](../../shaped-graphics/docs/shaders.md).
sv's generated material shaders carry none.
Emitting them from the generator would make the generated half portable and leave the hand-written `.hlsli` library to follow; a genuinely portable shader language is the larger follow-up.
The whole sv API compiles everywhere, though: without a backend a routine simply acquires no shader and draws nothing.

## First library-extension seams (per the "living libraries" rule)

- **The BSDF is sv's own**, in `shaders/openpbr.hlsli`: the OpenPBR Surface subset the path tracer shades through, plus the GGX / Fresnel / sheen primitives under it.
  A shared shader BRDF library in shaped-rendering is the natural home once a second consumer appears, and the primitives are the half that would move.
  The flat `pbr_raytrace_routine` still has its own `shaders/pbr.hlsli`, which is one of the reasons to retire that routine.
- **The shader-side `sv::` namespace is provisional**, and a new shader type should not land at its top level by default.
  It replaced the old `sv_` prefix and now holds four unrelated groups flat: the microfacet and Fresnel primitives
  (`ggx_*`, `fresnel_*`, `sheen_*`, `oren_nayar`, `dispersive_ior`, `thin_film_reflectance`, `luminance`), the shading frame
  (`frame`, `make_frame`, `to_local`, `perturb_frame`, …), the material model (`surface`, `bsdf`, `bsdf_*`, `medium_*`), and
  the generated-shader runtime (`instance`, `attribute_desc`, `shading_context`, `interpolate_*`).
  Names a layer above or below will want are already in it — `sv::surface`, `sv::luminance`, and `sv::frame`, which is an
  orthonormal shading basis in HLSL and the immediate-mode frame in C++.
  The split is deliberately deferred rather than skipped: it wants doing together with the move of the primitives to
  shaped-rendering above, since that move decides which group leaves and what the rest is named around.
- **Meshing primitives belong in typed-geometry.** Both examples hand-roll their geometry: a cube in `hello-cube`, a UV sphere in `openpbr-spheres`.
  Two callers is enough to want a `tg::` sphere / box tessellation.
- **The id-pool now exists** as `sv::impl::lru_pool<Id, Record>` (budget + idle eviction, LRU).
  If a second library wants it, promoting a generational version into clean-core is the next step.
- **TLAS is rebuilt every frame**, since refit/update is not implemented in sg yet; `tlas_id` exists for a future prebuilt/persistent TLAS.
- **Texture download** exists in sg as `cmd.download.bytes_from_texture`, but the trace stays on the proven UAV-write-then-blit path.
  Two tests read pixels back through it now — `volumetric-furnace-test` and `openpbr-bsdf-test` — and both are skipped on Windows on ARM, where that path fastfails.
  See the known issue in [TODO.md](TODO.md).
- **Compositing is a raster blit, not a compute copy**, and that is sg's constraint rather than a preference.
  A swapchain backbuffer is created render-target-only (`DXGI_USAGE_RENDER_TARGET_OUTPUT`, no UAV), and there is no GPU texture-to-texture copy.
  A compute compositor would have to write an offscreen UAV and then blit *that* to the backbuffer — one extra full-screen pass.
  Revisit if sg grows UAV-capable swapchains; note that a blit also converts format and filters, which a copy never will.
- **One mesh per view** — the trace binds the first item's vertex/material buffers; multi-mesh wants per-instance material/vertex indexing.
- **Transforms now come from typed-geometry**: `scene_item::transform` is a `tg::affine_transform3f`, and the hand-rolled `sv::impl::rotation` / `translation` builders are gone.
  A placement is built from tg's factories and `tg::compose`; the renderer reads its linear part and translation straight into the TLAS instance's row-major 3x4.
- **The per-view cache now exists** as `sv::impl::keyed_cache<Key, Record>` — externally keyed, two-tier idle reclamation, byte and entry budgets.
  It is the counterpart to `lru_pool` for state that is *named* rather than content-addressed.
  sv now carries two eviction implementations, which is one too many: folding `lru_pool` onto `keyed_cache` (it is `keyed_cache` plus minted ids plus a content-hash index) is the open follow-up.
- **`tg::angle` gained compound assignment** (`+=` / `-=` / `*=` / `/=`) — it claims to be a 1D vector space and the orbit controller wanted to accumulate into one.
  Still missing there: a scalar `pow` (the zoom falls back to `<cmath>`) and a scalar `abs`, which the controller's test works around locally.
- **`sr` has no input-state snapshot** — `sr::key_event` carries no cursor position and there is no held-button query, so the viewer tracks `last_cursor_pos` itself.
  An `sr::input_state` (cursor, held buttons, held modifiers as of the last poll) is the right home for what every consumer otherwise rebuilds out of latch bools.
- **`render_routine` has no per-frame hook**, and no way to reach an instance under its lock without a `command_list`.
  It no longer bites here: the per-frame reclaim moved onto `sv::view_store`, which needs no routine at all.
  A routine that does grow per-frame state would still want an init-free `acquire_exclusive(context&)` in sg.
- **`render_routine` exposes no reload counter**, so `view_renderer` counts its own `init_declare` calls to know when an accumulated image stopped being comparable.
  A `reload_generation()` on the base would be the shared answer, since any routine caching derived results across frames needs the same signal.
