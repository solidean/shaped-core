# shaped-viewer TODO

Running list of known follow-ups.
Bigger design intent lives in [structure.md](structure.md).

## The layout-tree restructure — what is left

The rendering half is **done and joined**: a frame is flattened into a `render_plan`, `view_renderer` allocates what it
names and records its traces, and `viewer_renderer` replays it into one `layout_routine` pass per texture.
Nesting renders at any depth (`tests/viewer-renderer-test.cc`), and `sr::blit_routine` is no longer used by sv.

The authoring API has landed too: `sv::interactive(id)` owns its viewer for the loop's lifetime, and the
`view_api` / `window_api` mixins make `f.add_scene()` and `f.window().view().add_scene()` the same call.
Handles are non-owning `{frame*, index}` pairs and nesting is expressed by holding one, so nothing needs closing and
there is no open-container stack.

What is left is the interaction on top of it, in dependency order:

- **The zoom cannot pan.** Ctrl+wheel magnifies at the cursor, which reaches anywhere by zooming out and back in, but
  there is no way to slide a magnified window at a fixed zoom.
  `view_state::zoom_center` is what a drag would write; picking a binding that does not collide with orbit (plain
  left-drag) or camera pan (middle-drag) is the open question.
- **A lifted view cannot be resized, or put back.** `view_ref::movable` + Ctrl+left-drag floats a view over the window,
  but there is no grab handle to resize it and no gesture to drop it back into the flow.
  Clearing `view_state::placement_seeded` is all "put it back" needs; the open question is the gesture.
- **A lifted view always floats over the whole window**, never inside the container it came from.
  That is deliberate — the caller rebuilds the tree every frame, so the only stable reference box is the window — but a
  caller who wants it re-parented into a specific container has no way to ask.
- **The UI layer** through `sr::imgui_context` / `sr::imgui_routine`, into the view's own target.
- **A second window**, which is only an sv-side step: `sr::window_system` already drives N windows from one poll.
- **`scene_2d` draws nothing**, and is typed and documented that way on purpose.
  shaped-core has no 2D renderer at all — no vector, sprite, text or path rasterizer in sv, sr or sg — so this layer
  needs one built before it can mean anything.
  That is its own project, not part of this restructure.
- **A traced layer has no alpha.** `pathtrace.hlsl`'s raygen writes none, so a `scene_3d` layer is forced to
  `layer_blend::replace`. Writing coverage into `.a` is what would let a traced layer composite `over` another.
  Until then `view_ref::add_scene` can express two scene layers on one view but only the last is visible.
- **`temporal_input` is declared but not honoured.** `view_data` carries the list and the reset-hash rule, and the
  renderer's record already holds a slot vector — but the path tracer's accumulation is still allocated as a special
  case rather than as the one temporal input a `scene_3d` layer declares.
  Folding it over removes a special case; it does not add a feature.
- **One traced layer per view is still assumed** in `view_renderer::execute`, the single-view convenience the GPU tests
  drive.
  The plan and `trace` are already per `(view, layer)`.

## Everything else

- Define the dev-friendly renderer/scene API once shaped-rendering provides enough of the underlying render routines.
- Replace the placeholder rotating cube: `scene_ref::add_mesh` authors real geometry now, but `viewer::finish_frame`
  still injects a spinning cube into any scene layer that has none.
- **A view's display name is stored and never drawn.** `impl::view_state` keeps it (defaulting to the id up to its `##`) for the title bar a view has no way to draw yet —
  that needs the 2D/text renderer the `scene_2d` entry above is waiting on.
- **`per_edge` attributes need an edge table on `triangle_geometry`.**
  The enumerator exists and `mesh_attribute::create` rejects it; what is missing is the numbering — the edges themselves (each naming its two vertices) plus each triangle's three edge indices.
  That table also decides whether opposite half-edges share one entry, which is the real design question.
- **An `sv::mesh` is authored but not rendered as one.** `scene_ref::add_mesh` takes one and translates it — geometry through `triangle_data::from`, per-face PBR through the
  `sv::pbr_attribute` lists — but what reaches the trace is still a `scene_item` naming two manager ids.
  What is missing is the material *definition* the `material_id` names, plus general attribute upload; only the four PBR fields cross to the GPU today, and only because the
  closest-hit already reads a `pbr_material_gpu` per triangle.
- **`mesh_attribute` cannot hold a struct.** `attribute_format` is a scalar plus a dimensionality, so a `pbr_material` array must be scalarized into four `per_triangle`
  attributes (`sv::pbr_material_attributes`).
  A struct protocol — a field list of scalar/vector members, so one attribute carries one AoS payload — deletes that function and the four blessed names with it.
- **`sv::pbr_attribute`'s names are a stand-in.** A material definition should declare which attributes it samples; there is no such type, so the repack in
  `material_manager::acquire` looks up four fixed names instead.
- Fold `lru_pool` onto `impl::keyed_cache` — it is `keyed_cache` plus minted ids plus a content-hash index — so sv carries one eviction implementation rather than two.
- Give `view_ref` a conditional-override vocabulary (an `ImGuiCond`-style `when { always, first_use }`), so `camera` /
  `resolution` / placement stop needing a separate `initial_*` setter each.
  Only the first-use half is built today.
- Let a view pick its controller: `sv::viewer` drives an orbit controller per view, so `fps_camera_controller` is only reachable by a caller running its own event pump.
  A view losing the cursor or the window losing focus must reach the controller's `release_input`, or a held key keeps flying.
  `pathtraced-window-manual-test.cc` still hand-rolls its own fly camera and can drop it once this lands.
- `render_settings::max_accumulated_frames`, replacing the file-scope `accumulation_frame_cap` in `view_renderer.cc`.
  It must then be excluded from the trace hash — lowering the cap should not restart the image.
- Multi-window compositing (multi-view within one window is done; the window system is one-per-process, so this needs shared ownership across viewers).
- Plan the RTX / ray-tracing path against the shaped-graphics backend capabilities as they land.
- Grow the [cheat-sheet](../cheat-sheet.md) + [structure](structure.md) as the renderer takes shape.
- **`mesh_is_indexed` belongs on the mesh, not the frame.**
  Geometry layout is a per-BLAS property.
  It rides in `frame_constants_gpu` / `pt_frame_constants_gpu` only because the trace binds one mesh per view.
  That is the same reason `Vertices` / `Indices` / `Materials` are single global bindings.
  Fold it into the per-instance mesh descriptor table the "one mesh per view" seam wants anyway, indexed by `InstanceID()` and carrying each mesh's vertex/index range or bindless handles.
  Moving the flag alone would not help: a per-instance flag over a still-global vertex buffer is no more correct.
  The DXR-native alternative is per-geometry data in the hit-group shader record via a local root signature, which specializes the `[branch]` in `mesh.hlsli` away.
  It needs local-root-signature support in sg's shader table first.
