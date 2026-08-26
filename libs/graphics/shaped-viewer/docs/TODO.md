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
  The same is true of `geometry_coat_normal`, which lands through the authored frame beside it.
- **`transmission_scatter` and `transmission_scatter_anisotropy` are absent**, which is now the ONLY missing parameter
  group.
  The machinery is all there — the integrator walks the subsurface interior with a phase function already — so this is
  giving the transmissive interior a scattering albedo of its own rather than building anything.
- **The thin film could stop aliasing now.**
  Its higher orders alias because it samples three wavelengths, and the path can be collapsed onto one wavelength since
  dispersion landed.
  Evaluating the film at the collapsed wavelength instead of at all three is most of what Belcour and Barla's spectral
  formulation would buy, for a path that has already paid the collapse.
- **Nothing tests the WALK.**
  The probe pins which interior a sample enters, which is the seam the integrator switches on — but the distance sampling,
  the phase function and the channel-averaged weighting all live in `pathtrace.hlsl` and are exercised only by running an
  example.
  A furnace test for a medium is the analogue of the one for the closure: a non-absorbing scattering sphere under a uniform
  environment must return the environment, whatever its albedo and mean free path.
- **A scattering walk is capped at 256 events and has no Russian roulette.**
  The cap is a termination guard rather than a quality control, and a dense medium with a high albedo will reach it — which
  is energy lost rather than a path fairly ended.
  Roulette is what would end those paths without bias, and it is the same mechanism the surface bounces want.
- **Next-event estimation never happens inside a medium.**
  A scattering event turns and continues, picking up light only when it eventually exits, so a lit interior converges far
  more slowly than a lit surface does.
  It is the same gap as the shadow ray that cannot pass through glass, and equidistant sampling toward the light is the
  usual answer.
- **Dispersion collapses the whole path onto one wavelength**, and keeps it collapsed.
  Everything after the collapse costs three times the samples for the same noise, including surfaces that have nothing to
  do with the dispersive one.
  Collapsing only the transmitted lobe's continuation — leaving a reflected one carrying all three — is what would bound
  that, and it needs the closure to say which lobe a sample came from beyond the interior it entered.
- **Next-event estimation does not pass through glass.**
  A shadow ray is a visibility test with no closure on it, so a point inside a transmissive solid — or behind one — is lit
  by BSDF sampling alone.
  That is what makes a caustic converge slowly and a glass interior noisy.
  The fix is a shadow ray that accumulates transmittance instead of stopping at the first hit, which needs the any-hit to
  serve two purposes or a second traversal.
- **The transmitted lobe drops the radiance-compression factor**, deliberately: this tracer transports importance from the
  camera rather than radiance from the light, and the two conventions differ by exactly the square of the index ratio.
  A renderer that ever grows a light-side path — bidirectional, photon mapping — has to put it back on that side.
- **A ray escaping while still inside a solid is dropped.**
  It travelled an unbounded distance through an absorbing medium, so nothing survives — but that is only true for CLOSED
  geometry, and an open shell authored as solid loses paths rather than being told it is wrong.
- **Total internal reflection ends the sample rather than reflecting.**
  `bsdf_sample_direction` returns invalid, and the reflected lobe carries that energy through its own Fresnel — so the
  energy is accounted for but a path that should have bounced inside the solid terminates instead.
  It is what makes a thick glass corner darker than it should be.
- **The thin film is sampled at three wavelengths**, one per output channel, rather than integrated over the spectrum.
  Its first interference order is faithful and its higher orders alias into colors the spectrum would have averaged away, so
  a film past roughly a micron drifts.
  Belcour and Barla's spectral formulation is the replacement, and it wants the same hero-wavelength machinery dispersion
  does — which is why the two are worth doing together rather than separately.
- **The film's Fresnel does not reach the layer coupling.**
  `spec_transmission` still reads the film-free `spec_f0`, so what the diffuse substrate is charged for the crossing ignores
  the interference above it.
  Averaged over the spectrum a film redistributes reflectance rather than adding it, so the error is small — and closing it
  wants the same tabulated albedo the energy compensation does.
- **The film treats the base as a real index.** A metal's absorption therefore does not shift the phase it reflects with,
  which costs the slight hue rotation a real conductor's substrate adds.
- **The coat's own normal is added as a BSDF about the BASE normal.**
  `geometry_coat_normal` gives the coat its own frame and every cosine its lobe needs is measured there, but the result is
  summed into a closure the integrator weights by the base's cosine.
  The two frames disagree by a ratio no closed form absorbs; the alternative is a second integrator, and every renderer that
  carries a coat normal makes the same trade.
- **`geometry_coat_tangent` is absent.** An anisotropic coat is stretched along the base's tangent spun onto the coat's
  normal, so it follows the same uv layout the base does and cannot point its own way.
  It is `geometry_tangent`'s mechanism a second time over, and nothing needs it yet.
- **A cutout is stochastic and its draw does not come from the path's own stream.**
  `PtAnyHit` hashes the pixel, the frame seed and the primitive instead, because an any-hit writing the path's random state
  would have to be granted access to it — and every ray would then carry a stream whose length depends on how many
  alpha-tested triangles it happened to graze.
  Independent draws per bounce are what it costs, which accumulation hides and a single-sample preview would not.
- **No test traces a material that can cut out.**
  `material-shader-cache-test` compiles the any-hit for a type that writes `geometry_opacity`, so the HLSL is covered; what
  is not is a pipeline built with an any-hit attached and a trace through it.
  Every tracing test drives the glTF type, which never writes opacity and so deliberately gets no any-hit.
- **A GGX sample that reflects below the horizon is dropped rather than redistributed**, so `bsdf_pdf` legitimately claims
  less than the full hemisphere — around a tenth of it for a rough lobe.
  That is unbiased and standard, and the probe asserts the direction that matters (never MORE than 1) rather than equality.
  Multiple-scattering GGX sampling is what would put that mass back, and it is the same tabulated-albedo work as the
  compensation entry below.
- **Three lobes are approximations, named at the top of `openpbr.hlsli`.**
  The fuzz is a Conty-Estevez sheen rather than the specified Zeltner microflake, the coat tints what passes through it once
  rather than absorbing along the refracted path, and GGX energy compensation is Turquin's analytic fit rather than a tabulated
  directional albedo.
  Each is a self-contained replacement, and the sheen is the one that most visibly deviates.
- **The closure is measured; the IMAGE still is not.**
  `shaders/bsdf_probe.hlsl` plus `tests/openpbr-bsdf-test.cc` run three estimators over `sv::bsdf` on the GPU and read the
  numbers back — directional albedo, the mass `bsdf_pdf` claims against what `bsdf_sample_direction` draws, and Helmholtz
  reciprocity — across eleven surfaces at three incidences each.
  A lobe added to the closure goes in `surfaces_under_test` and is then held to all three.
  What that does not cover is anything above the closure: the integrator, the accumulation blend and the layout composite
  still have no readback, and `pathtraced-window-manual-test` is the only confirmation of those.
- **Two of the energy bounds are a fit's error rather than a lobe's.**
  A white metal furnace overshoots by about 3% (Turquin's analytic compensation) and the fuzz by about 6% at grazing
  (`sheen_albedo` charges the layers below it less than the Conty-Estevez lobe actually reflects).
  The probe's `1.06` energy bound is exactly those two, so it is what tightens when the tabulated albedos above land.
- **The environment cannot produce a sharp reflection.** The background is an order-3 SH probe, so a smooth specular lobe
  reflects a blur whatever the roughness says; only the analytic area light gives a real highlight.
  An equirect HDR environment with 2D-CDF importance sampling is the fix, and it needs a Radiance `.hdr` reader in babel first —
  `babel::image` has PNG and JPEG only.
- **An emissive mesh lights nothing but the camera.** `emission_luminance` is authored and shaded, but next-event
  estimation samples the analytic area light alone, so emissive geometry is direct-visibility only and contributes no
  indirect light.
  What it needs is light sampling over emissive triangles — an emitter list built per trace with its own area pdf,
  balanced against the BSDF sampler the way `pt_light_intersect` already balances the rect.
- **A partly-covered surface is expressible now**, through `PtAnyHit` in `shaders/pt_material_hit.hlsli`.
  It is attached only to permutations whose material actually writes `geometry_opacity` (`material_permutation::can_cut_out`),
  because a hit group carrying an any-hit gives up the hardware's opaque fast path for every intersection on it.

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
- **A view picks its controller now** — `view_ref::camera_style`, with `sv::viewer` routing to it and driving the fly one's
  `update(dt)`, releasing input while the window is unfocused.
  What is left of that entry is the cleanup: `pathtraced-window-manual-test.cc` still hand-rolls its own fly camera and can
  drop it.
  Losing the CURSOR is also still unhandled — only focus is — so a view whose cursor leaves the window mid-look keeps looking.
- **Nothing tests `camera_style`.** The controllers have their own CPU tests, but the switch, the re-seeding across it and the
  unfocused release all live in `sv::viewer`, which needs a window — so they are only exercised by running an example.
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
