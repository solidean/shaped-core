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
- **The accumulated image is never read back.** The tests pin the *policy* — a camera or scene change restarts the
  counter, a relayout does not — but nothing checks that the blend itself produces the running mean it claims to.
  The cheap check is a readback test: trace a converging static view twice at a known constant color and assert the
  target holds the mean rather than the last frame.
  Until that exists, `pathtraced-window-manual-test` is the only real confirmation.
- **A camera move throws the whole estimate away**, which is the deliberate trade behind an uncapped exact mean, and
  it is what makes flying noisy.
  Reprojecting the history through the previous camera and rejecting per pixel is the classical answer and was tried;
  it cost a G-buffer, a ping-pong pair, a disocclusion heuristic and a per-pixel sample count, and it capped the mean.
  A spatial filter over a moving frame (A-trous / SVGF) buys the same smoothness without touching the estimator, and
  is the direction to take this if flying needs to look better.
- **The GPU tests may still be passing vacuously.** `pathtrace_routine::init_declare` used to drive its shader compiles
  with a throwaway single-threaded scheduler, which could not complete a node the ambient pool already owned — so the
  routine ended up with no pipeline and `execute` silently no-opped.
  `cc::try_async_blocking_get` waits on the scheduler that owns the node, which removes that failure mode.
  What remains is the coverage gap it exposed: every tracing test but `pathtraced-view-test` asserts only CPU-side
  facts, so none of them would notice tracing nothing at all.
  Until then, a tracing test that means anything needs `nx::config::main_thread` *and* an `is_ready` assertion.
  That assertion now reports the last trace rather than the routine, so it belongs AFTER the execute rather than before it.
- **One traced layer per view is still assumed** in `view_renderer::execute`, the single-view convenience the GPU tests
  drive.
  The plan and `trace` are already per `(view, layer)`.

## What the bindless lock does not cover

`sv::gpu_resource_manager` holds the lock, and its header carries the same two entries.
They are recorded rather than solved: the lock is sound, but it is not what makes an index valid.
What keeps a live index from being reassigned is sg's reclaim rule — a full array reclaims only indices *not acquired this epoch* — which is structural and needs no lock.

- **The lock prohibits where a generation stamp would verify.**
  Bump a counter on every mint and record it in `bound_resources`.
  Asserting at bind time that the snapshot covers every index the recording used would *check* the invariant rather than forbid its violation — same cost, and a failure that names what went wrong.
- **The lock is global where the hazard is per-recording.**
  A routine that legitimately wants to acquire mid-recording — a stream just landed, a later trace in the same list wants it — has one correct answer.
  That answer is a fresh snapshot, rebound for the dispatches after it.
  A clean `snapshot()` is the cached handle, so it is nearly free.
  The lock refuses instead.

## What the material system still needs

The chain is joined end to end.
A `sv::mesh` names a material, `scene_ref::add_mesh` resolves it against the mesh, and `gpu_resource_manager` generates and compiles its permutation and fills its parameter block.
`pathtrace_routine` then traces a DXR pipeline carrying one hit group per permutation.
What is left is narrower than it was:

- **slib has no named-HLSL-fragment asset kind.**
  A material type's `shader` is a fragment, not a compilable shader, so the builtins carry theirs as string literals in `material/builtin_material_types.cc`.
  Moving them under `shaders/` once slib can declare a fragment gets editor support and hot reload.
- **Two permutations may not disagree about a sampler register.**
  A generated source names `sv_sampler_0` at `s0` and the pipeline bakes the states in as name-matched static samplers, so one register is one state for the whole pipeline.
  The DXR-native answer is a per-hit-group *local* root signature, which sg's shader table does not carry yet.
  Until it does, `collect_samplers` asserts when two materials claim one register with different states — loudly on the dev box, rather than an image nobody can explain.
  Two materials sampling the same way still share it silently, which is the case that is actually fine.

- **A generated permutation does not hot-reload when an `.hlsli` it includes is edited.**
  The generated source carries a literal `#include` line whose bytes never change when the file does.
  `material_shader_key` hashes the resolution and the generation options, never the include's contents.
  Folding `slib::current_reload_generation()` in is not the fix: it is bumped for any watched file, so it would rebuild every permutation rather than the ones that changed.
  What is needed is for `shader_library::compile_source` to return its `outcome.dependencies`, which it already computes and discards.
  The cache can then hash the resolved include contents and rebuild precisely what moved.

- **A sampled attribute cannot say what it is sampled THROUGH.**
  `resolved_attribute::uv` is one hardcoded field: a `float2` mesh attribute, found by name, and nothing else may play that role.
  The generalization is a *dependent* attribute: one that declares which other attribute supplies its coordinate.
  A 1D coordinate into a 1D texture, a second uv set and triplanar then become the same mechanism rather than three special cases.
  It is a change to `resolved_attribute`, to the two shape-side hashes in `resolve_material`, and to the slot the generator emits for it.
- **One buffer per parameter block.**
  That is one bindless slot per distinct (material, mesh) pairing rather than per instance, which is affordable but not free.
  Packing many blocks into one buffer is invisible to the shader — it already takes an offset — so it is an optimization rather than a change of contract.
- **A parameter block is rebuilt every epoch, per distinct (material, mesh) pair.**
  That is tens of bytes of CPU work per pair per frame, and it buys an access declaration correct by construction: every index a hit reads is minted by the call that writes it.
  The buffer it is written into stays *persistent*, and is re-uploaded only when the bytes change.
  A fresh transient buffer per frame mints a descriptor per frame, which leaves the staging group permanently dirty and re-mints the whole bindless table on every trace.
  That exhausted the descriptor heap within a second of `interactive-showcase-manual-test`, so the persistence is load-bearing rather than incidental.

- **Nothing evicts a parameter block.**
  The LRU pool went away with the pin it existed to hold, so the set is bounded by the distinct (material, mesh) pairs a process ever draws rather than by a budget.
  Each is one small buffer plus its slots; a long-lived session cycling through materials would grow without bound.
  Folding the records onto `impl::keyed_cache` alongside the other managers is what would bound it.

- **A pipeline is keyed on the whole permutation SET, in scene order.**
  Two views whose scenes hold the same materials in a different order build two pipelines over the same shaders.
  Sorting the set before keying it would collapse them, at the cost of a `hit_group_offset` that no longer follows first use — worth doing once a scene has enough materials for it to matter.
- **The generator handles scalars and vectors of f32 / i32 / u32 only.**
  A matrix attribute has no settled `ByteAddressBuffer` layout here, and the narrow and 64-bit scalars need SM 6.2 16-bit types or a split load.
  `hlsl_type_of` returns empty for those and `generate_material_shader` asserts, rather than emitting something that will not compile.
- **The accumulation hash can restart a converged image for something that is not a scene change.**
  `trace_hash` hashes each record's `vertices` and `indices` bindless indices plus its BLAS pointer.
  Those are stable while a working set is.
  They move the moment something reclaims — a bindless array that fills and rotates elements, or a mesh evicted from `mesh_manager` and re-uploaded under a new BLAS handle.
  Same scene, restarted accumulation, nothing reporting it.
  The rule the fix has to keep is that the hash may be imprecise in **one direction only**:
  a render setting that feeds the hash without changing the image — a "high quality mirror" toggle in a scene with no mirrors — costs a restart nobody minds,
  while a restart on a static scene the user did nothing to is not acceptable.
  Precise content hashing for accumulation is what eventually fixes it.
- **A library hook has no reset, and whether it should have one differs per hook.**
  `acquire_shader_library`, `acquire_material_library` and `acquire_context` each memoize the first *successful* acquire into a function-local static that nothing clears.
  So the first success decides for the process.
  slib's constraint is real and external — a second `slib::shader_library` would fight the first over the package globals both write into — so a shader-library reset is not obviously legal.
  The material library is scene state with no such constraint.
  Whatever lands has to account for tests running concurrently.
  `material-resolution-test.cc` already installs its own material library process-wide without `nx::exclusive`, hedging around the race with `CHECK(builds <= 1)`.

## What the OpenPBR surface still needs

`sv::surface` is OpenPBR's parameter set and `shaders/openpbr.hlsli` is the layered BSDF over it: fuzz over coat over a base that
mixes the metal against a dielectric specular layer over the diffuse substrate.
The path tracer shades through it — the closest-hit evaluates the closure, estimates both light sources through it, and
importance-samples the continuation — so what is left is coverage of the model rather than plumbing.

- **Tangent frames are quaternions, and not yet quantized.**
  A mesh supplies `tangent_frame` — a unit quaternion taking tangent space to object space — plus `tangent_handedness`, and the
  hit builds its shading frame from that instead of the flat face normal.
  The storage is an uncompressed `f32x4`.
  The intended encoding is `quat10x3+i2` (see [zeux.io on quantizing tangent frames](https://zeux.io/2026/04/30/quantizing-tangent-frames/)),
  which is a format change on one attribute plus a decode in the generated prologue — the same seam `attribute_interpolation`
  already opened, and not a content migration, because handedness deliberately lives beside the quaternion rather than in the
  sign of its `w`.
  `quat10x3+i2` rather than the article's own `oct11x2+d9` pick, because a closest-hit decodes three corners per hit and then
  blends them: the quaternion is the form the blend wants, where an octahedral normal plus a diamond angle would have to be
  built into a basis per corner first.
- **A non-uniform instance scale shears the tangent frame.**
  The hit rotates the authored frame by `ObjectToWorld3x4` and renormalizes, which is exact for a rigid or uniformly scaled
  placement and wrong for anything else — the normal wants the inverse transpose while the tangent wants the matrix itself.
  Nothing in the tree scales non-uniformly yet.
- **Nothing produces a frame but the sphere example.**
  `openpbr-spheres` emits an analytic one per vertex.
  A glTF import would carry `TANGENT` and a handedness in its `w`, and a mesh with uvs but no tangents wants them derived
  rather than defaulted — neither exists.
- **A normal map is still untested.** `geometry_normal` is applied through `sv::perturb_frame` now, so it has somewhere to land,
  but no material in the tree binds it to a texture.
- **Transmission, subsurface, thin-film and dispersion are absent**, not defaulted — `sv::surface` does not carry them, so a
  material cannot ask for one and silently get something else.
  Transmission is the one that changes the integrator rather than only the closure: it needs a refracted continuation and
  therefore a path that leaves the upper hemisphere.
- **Three lobes are approximations, named at the top of `openpbr.hlsli`.**
  The fuzz is a Conty-Estevez sheen rather than the specified Zeltner microflake, the coat tints what passes through it once
  rather than absorbing along the refracted path, and GGX energy compensation is Turquin's analytic fit rather than a tabulated
  directional albedo.
  Each is a self-contained replacement, and the sheen is the one that most visibly deviates.
- **No pixel is ever inspected.** The suite pins that the permutation compiles, that the pipeline builds and that the trace
  runs; nothing checks what the BSDF returns.
  The cheap first assertions are a white-furnace test — a rough metal under a uniform environment must return roughly its own
  albedo — and a reciprocity check over sampled direction pairs.
  Both need the pixel readback the accumulation entry above also wants, which is the one piece of test infrastructure sv has
  none of.
- **The environment cannot produce a sharp reflection.** The background is an order-3 SH probe, so a smooth specular lobe
  reflects a blur whatever the roughness says; only the analytic area light gives a real highlight.
  An equirect HDR environment with 2D-CDF importance sampling is the fix, and it needs a Radiance `.hdr` reader in babel first —
  `babel::image` has PNG and JPEG only.
- **An emissive mesh lights nothing but the camera.** `emission_luminance` is authored and shaded, but next-event
  estimation samples the analytic area light alone, so emissive geometry is direct-visibility only and contributes no
  indirect light.
  What it needs is light sampling over emissive triangles — an emitter list built per trace with its own area pdf,
  balanced against the BSDF sampler the way `pt_light_intersect` already balances the rect.
- **`geometry_opacity` is parsed and ignored.** Nothing in the trace has an any-hit shader, so a cutout is not expressible yet.

## Everything else

- Define the dev-friendly renderer/scene API once shaped-rendering provides enough of the underlying render routines.
- **A failing `CC_ASSERT` inside the frame loop turns into `std::terminate`**, not a test failure.
  nexus reports the assert by throwing, the stack unwinds through `viewer::~viewer`, and `advance_epoch` asserts
  again on the way out — a second assert during unwinding is an immediate abort.
  So any assert reached from a viewer frame loses its own message behind an `abort()`, which is what made the empty
  scene layer above expensive to find.
  The viewer's destructor should be able to tear down a viewer whose frame did not complete.
- **A view's display name is stored and never drawn.** `impl::view_state` keeps it (defaulting to the id up to its `##`) for the title bar a view has no way to draw yet —
  that needs the 2D/text renderer the `scene_2d` entry above is waiting on.
- **`per_edge` attributes need an edge table on `triangle_geometry`.**
  The enumerator exists and `mesh_attribute::create` rejects it; what is missing is the numbering — the edges themselves (each naming its two vertices) plus each triangle's three edge indices.
  That table also decides whether opposite half-edges share one entry, which is the real design question.
- **`mesh_attribute` cannot hold a struct.** `attribute_format` is a scalar plus a dimensionality, so an array of PBR values has to be authored as one attribute per field.
  A struct protocol — a field list of scalar/vector members, so one attribute carries one AoS payload — would let a caller hand over the array they already have.
- Fold `lru_pool` onto `impl::keyed_cache` — it is `keyed_cache` plus minted ids plus a content-hash index — so sv carries one eviction implementation rather than two.
- Give `view_ref` a conditional-override vocabulary (an `ImGuiCond`-style `when { always, first_use }`), so `camera` /
  `resolution` / placement stop needing a separate `initial_*` setter each.
  Only the first-use half is built today.
- Let a view pick its controller: `sv::viewer` drives an orbit controller per view, so `fps_camera_controller` is only reachable by a caller running its own event pump.
  A view losing the cursor or the window losing focus must reach the controller's `release_input`, or a held key keeps flying.
  `pathtraced-window-manual-test.cc` still hand-rolls its own fly camera and can drop it once this lands.
- `render_settings::max_accumulated_frames`, replacing the process-wide `sv::accumulation_frame_cap` in `render_settings.hh`.
  It is public rather than file-scope because a caller waiting for convergence has to tell "not there yet" from "as good as it gets" — `view_ref::is_accumulation_converged` applies it.
  It must then be excluded from the trace hash — lowering the cap should not restart the image.
- **`shaped-viewer/hello-cube`'s reference image shows one of its six face colours.**
  The example declares a `base_color` per face and spends half its body building them, which is the thing a reader opens it to understand.
  In `examples/hello-cube.jpg` the overhead light at emission 14 blows the top face to white, and the third visible face is lit by the sky alone and reads as pale grey rather than purple.
  Only the red face survives.
  Turning the light down and lowering the elevation a little would put three distinguishable colours in frame; `uv run dev.py example shaped-viewer/hello-cube --capture` is the loop for it.
  Left as it is on purpose — the image shows the viewer working, which is what it is mainly for.

- **The default shader library is leaked on purpose, and the shape that forces it is not settled.**
  `impl::acquire_default_shader_library` allocates an `slib::shader_library` it never frees.
  The generated package symbols an asset is reached through are process-wide globals, so destroying the library at exit would leave them naming freed assets.
  So the lifetime is dictated by slib's package globals rather than chosen here, and sv is where the consequence lands.
  The construction is wrapped in a `cc::leak_scope` so LeakSanitizer reads the decision instead of reporting it, which is an annotation and not a fix — it also hides any real leak in that window.
  What would settle it is slib owning the library's lifetime itself, with the package globals resolving through it rather than caching raw pointers into it.
  Not urgent: one library per process is slib's rule either way, so the leak is bounded and constant.

- Multi-window compositing (multi-view within one window is done; the window system is one-per-process, so this needs shared ownership across viewers).
- Plan the RTX / ray-tracing path against the shaped-graphics backend capabilities as they land.
- Grow the [cheat-sheet](../cheat-sheet.md) + [structure](structure.md) as the renderer takes shape.
- **`mesh_is_indexed` still rides in `frame_constants_gpu`**, for `pbr_raytrace_routine` alone.
  The path tracer reads it per instance now, out of `instance_gpu`, and its own frame block no longer carries it.
  The flat routine keeps the global `Vertices` / `Indices` / `Materials` bindings `shaders/mesh.hlsli` declares, which is the reason the flag is still per frame there.
  Retiring it means giving that routine the same instance table, or retiring the routine.
