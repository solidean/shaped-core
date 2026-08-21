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
authoring API (interactive / refs)       [done]         sv::interactive -> frame -> window_ref -> view_ref -> layout_ref / leaf_ref / scene_ref.
                                                        A frame inherits the window surface, which inherits the view surface, so f.add_scene() == f.window().view().add_scene()
context provider (set_acquire_context)   [done]         an overridable hook, called at most once per process; the default brings up dx12 (hardware, then WARP)
input routing + key-bound zoom           [done]         picks the leaf under the cursor in painter's order through the plan's region links;
                                                        Ctrl+wheel magnifies what a leaf samples without touching a camera or a trace
mesh / triangle_geometry / attributes    [in progress]  the authoring-side mesh: triangle_geometry (raw or indexed, pinned + hashed) plus named attributes (per element, or per instance for a per-mesh value), a material id, flags and textures
resource managers (mesh / material)      [in progress]  strongly-typed ids -> GPU resources (BLAS built here); LRU budget + idle eviction
resource data (triangle / indexed / material)  [in progress]  what a caller uploads: a pinned_data payload + its cc::hash128 content key
bindless tables                          [planned]      sv owns none of the mechanism: sg::bindless_array maps views to element indices over a staging group the caller builds.
                                                        Nothing here uses one yet — the per-instance mesh table is the first consumer
pathtrace_routine                        [in progress]  the DXR GI trace view_renderer drives: TLAS + dispatch_rays into a UAV target
pbr_raytrace_routine                     [in progress]  the flat single-bounce IBL DXR trace (SH environment), driven directly
sv_shaders package                       [in progress]  raygen / miss+closest-hit, plus layout.hlsl (border / view / wipe), via slib
camera / controls                        [in progress]  dev-friendly pinhole camera, plus sv::orbit_camera_controller (event-driven) and sv::fps_camera_controller; only the orbit one is wired in
persistent per-view state                [in progress]  view_id keys what a view keeps — camera, controller, zoom, display name, last rect, composite target and accumulators — all in one sv::view_store the frame owns
id stack (push_id / scoped_id)           [done]         seeds view_id so one name under N scopes names N views; independent of layout nesting, and a duplicate within a frame asserts.
                                                        Ids are formattable and take an ImGui-style ## suffix, which separates two views without changing what a human reads
temporal accumulation                    [in progress]  a traced layer reprojects its history through the previous camera and blends per pixel; only a scene change restarts the whole image
materials / lighting                     [planned]      one flat PBR material; a scene layer holds typed light lists + an SH background; textures and more light kinds next
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
Vulkan RT is stubbed upstream in shaped-graphics, so rendering is Windows-only for now.
The whole sv API compiles everywhere, though: without a backend a routine simply acquires no shader and draws nothing.

## First library-extension seams (per the "living libraries" rule)

- **PBR/BRDF shading** is authored fresh in `shaders/pbr.hlsli`; a shared shader BRDF library is the natural home once a second consumer appears.
- **The id-pool now exists** as `sv::impl::lru_pool<Id, Record>` (budget + idle eviction, LRU).
  If a second library wants it, promoting a generational version into clean-core is the next step.
- **TLAS is rebuilt every frame**, since refit/update is not implemented in sg yet; `tlas_id` exists for a future prebuilt/persistent TLAS.
- **Texture download** exists in sg as `cmd.download.bytes_from_texture`, but the trace stays on the proven UAV-write-then-blit path.
  Pixel-level tests have not been written against it yet.
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
