# Asset loading (plan)

**Status: phases 1 to 3 landed, phase 4 half landed, the rest planned.**
The CPU / GPU type split is in — `sv::mesh_data`, `sv::mesh`, symmetric textures, `add_mesh` taking either.
So is the material mapping — `alpha_cutoff`, `occlusion` and the channel swizzle, through resolve, generation and the permutation key.
And so is the synchronous importer — `sv::asset_loader` over glTF, OBJ and STL, with the resolver hook.
Everything else here is still design, and the phasing at the end is what lands in which order.

The goal is one convenience layer: **load meshes from a file and draw them**, across glTF, OBJ, STL and whatever comes after,
with materials that survive the trip and can be overridden, and with individual meshes or materials selectable out of a file.

babel is what reads the formats.
sv is what turns a parsed document into things a view can draw, because materials, textures, instancing and tangent frames are
sv's vocabulary and babel must not grow one of its own.
So **shaped-viewer gains a public dependency on babel-serializer** — a legal direction (`data/` sits below `graphics/`), and the
first time sv links anything below shaped-rendering.

babel's own `load_mesh` aggregator stays on its roadmap for callers who want triangles without a renderer.
It is blocked on `tg::mesh`, it answers a different question, and neither supersedes the other.

## The two layers

The central decision is that a mesh exists in two forms, with different invariants, and that both are first-class.

| | `sv::mesh_data` (CPU) | `sv::mesh` (GPU) |
|---|---|---|
| holds | pinned bytes + content hash | owning refcounted resource handles |
| needs a resource manager to exist | no | yes |
| its GPU copy is | evictable, and always recoverable from the bytes | held resident by the handle |
| geometry produced by a compute pass | impossible | natural |
| identity | content hash | handle id |
| what it is for | the simple path, and everything before a device exists | performance, expansion, GPU-generated data |

`sv::mesh` becoming a bundle of handles is what admits geometry and attributes that were never on the CPU at all.
`sv::mesh_data` keeping pinned bytes is what lets the caching stay invisible: the bytes are a recipe that can always be replayed,
so an eviction is a re-upload rather than a correctness problem.

Naming follows [resources/resource_data.hh](../src/shaped-viewer/resources/resource_data.hh), where `texture_data` and
`material_data` already mean "the CPU payload of a resource".
So the CPU-side mesh is `mesh_data`, and `sv::mesh` is the GPU-side bundle.
Textures follow the same split, and `mesh_texture` holding `texture_data` on the CPU side is what removes today's asymmetry —
geometry travels as pinned bytes while a texture travels as an already-minted, evictable `texture_id`.

**One call takes either**, and the frame is what supplies the manager, so no global and no singleton is needed:

```cpp
auto const cube = sv::mesh_data::from_triangles(cube_triangles(1.0f));
for (auto frame : viewer.frames())
    frame.add_scene().add_mesh(cube);   // acquired through frame.resources(), keyed by hash, invisible

auto const m = res.create_mesh(...);    // handles, minted explicitly
scene.add_mesh(m);                      // nothing to look up
```

`mesh_data` may carry a **weak** cache slot — the manager it was last acquired against, and the handle it got — so a repeat
`add_mesh` is a pointer compare rather than a hash lookup.
It is weak rather than owning precisely because the bytes are retained: a stale slot falls back to the hash path, or to a
re-upload, and neither is unsound.

## Acquisition

Low-level factories live **on the resource manager**, since a GPU mesh cannot exist without one:

```cpp
sv::mesh res.create_mesh(sv::mesh_data const&);          // from CPU bytes
sv::mesh res.create_mesh(sv::geometry_handle);           // adopt a compute-produced buffer
sv::asset res.create_asset(sv::asset_data const&);       // a whole loaded asset, in one go
sv::geometry_handle res.create_geometry_from_triangles(cc::span<tg::triangle3f const>);
sv::buffer_handle res.create_attribute_buffer(...);
sv::texture_handle res.create_texture(sv::texture_data const&);
```

Format loading lives on a separate **`sv::asset_loader`**, which holds no device at all:

```cpp
auto loader = sv::asset_loader({.frames = ..., .resolve = my_vfs});
auto const car = loader.load("car.glb").value();          // an sv::asset_data — CPU, no manager, no viewer
```

Two reasons the loader is its own type rather than more methods on the manager.
It keeps babel out of `gpu_resource_manager` entirely, so the new dependency stays confined.
And load options are almost always shared across many loads, so a loader holding them beats repeating an options struct per call.

The loader being CPU-side is what buys the rest: loading on a worker thread, loading before a viewer exists, CPU-side mesh
processing after the load, and testing the whole importer with no device.

Every source is an overload rather than a mode flag:

```cpp
cc::result<asset_data> load(cc::string_view uri, ...);      // through the resolver, never the filesystem directly
cc::result<asset_data> load(cc::pinned_data<byte const>, asset_format, ...);
cc::result<asset_data> load(babel::gltf::data const&, ...); // already parsed
cc::result<asset_data> load(babel::obj::data const&, ...);
```

The already-parsed overloads are what let a caller who reads a document for their own reasons get meshes out of it without
re-reading, and they keep the babel document as the importer's actual input.

## Selection and overrides need almost no API

`asset_data` is plain data made of the types the scene API already takes, so filtering and overriding are code rather than options:

```cpp
struct sv::asset_data
{
    cc::string name;
    cc::vector<sv::mesh_data> meshes;          // flat, world-placed, one per (geometry, material)
    cc::vector<sv::asset_material> materials;  // file order: name plus the material_id minted for it
    cc::vector<sv::asset_node> nodes;          // the hierarchy, kept for callers who want it
    cc::vector<cc::string> issues;             // babel's issues plus the importer's own, forwarded
};
```

```cpp
for (auto const& m : car.meshes)
    if (m.name.starts_with("wheel"))
        scene.add_mesh(m);

car.override_material("glass", my_glass);   // rewrites every mesh bound to that slot
```

with `find_mesh(name)`, `meshes_with_material(name)` and `material(name)` as the convenience queries on top.

Load options then carry only what must happen *during* the parse, because it saves work or cannot be undone afterwards:
an `include_mesh` predicate, `import_materials`, `import_textures`, a `material_override` hook, `flatten_hierarchy`, and the
tangent-frame options below.

**One mesh per contiguous (geometry, material) pair**, whatever the format.
A glTF mesh with three primitives becomes three meshes; an OBJ's `usemtl` runs become one mesh per run, which
`babel::obj::data::materials` already hands over as spans.
It falls out of `sv::mesh` carrying exactly one `material_id`.

**The node hierarchy is flattened by default**, because instancing is already free here: geometry is content-hashed, so ten nodes
referencing one glTF mesh produce ten meshes with ten transforms over a single upload.
`asset_data::nodes` keeps the tree for callers who want it.

Imported material names are namespaced by the asset (`"car.glb/glass"`), since `material_library`'s name lookup is last-wins and a
convenience rather than an identity.
Content addressing still dedupes genuinely identical materials across files.

## The resource model: slots, states, recipes

A handle is not a resource.
It names a **slot** in the manager, and the slot holds a state, a payload that may not be there yet, a recipe for producing the
payload, and a small CPU-side summary that is known early.

```text
record = state (pending | ready | failed)
       + payload (the GPU objects; absent while pending)
       + recipe (how to produce or reproduce the payload)
       + summary (bounds, counts, extent, format)
```

This one indirection is what makes fallbacks, asynchronous loading and eviction-with-recovery the same mechanism seen from three
sides.
`sv::residency` already exists and means "how much of this resource has reached the GPU", with `resolve` answering a level so a
renderer can pick placeholder, base level or wait — so this **extends that concept to every resource kind** rather than adding a
parallel one.

### The recipe

A recipe is how a payload is produced, and therefore how it is reproduced after an eviction.

- **`from_memory`** — the bytes are pinned right here.
  This is what `mesh_data` and `texture_data` are, so the convenience path is not a separate mechanism but the trivial recipe.
- **`from_uri`** — a source uri, a sub-selector (`car.glb#mesh3.primitive0`, `car.glb#image7`) and the load options.
  Reproducible from nothing but itself.
- **`derived`** — an operation over other resource keys: mip generation, block compression, tangent-frame generation.
  Recursively reproducible.
- **`adopted`** — a compute-produced resource with no recipe at all.

Which gives the invariant the whole eviction story rests on:

> **A resource is evictable if and only if it has a recipe.**

No residency flag and no pinning policy: an adopted resource cannot be evicted because nothing could bring it back, and everything
else can be, because something can.
The manager must refuse to evict an adopted resource rather than dropping something unrecoverable.

Two requirements follow.
A recipe must be stable and hashable, so **the importer carries a version in every key** — change the tangent algorithm and get a
new key rather than a stale hit.
And a `failed` slot keeps its recipe, so a retry is possible when the cause was transient.

A slot can therefore go **ready to pending again** after an eviction.
The state machine is not monotonic, and the substitution paths below have to handle re-entry rather than only first load.

### Every step is cacheable

| step | key | cached where | after an eviction |
|---|---|---|---|
| fetch | uri | the resolver's business (VFS, OS page cache) | re-fetch |
| structure parse | file content hash + importer version | `bcache` | re-parse, which is cheap |
| payload decode and processing | source key + operation + params + importer version | `bcache` — where block compression and tangent generation pay off | re-run, usually a cache hit |
| upload | — | not cacheable | re-upload from the step above |

### Nothing is ever a hole in the frame

Every resource kind has a substitution for "not there yet", so a pending or failed resource degrades rather than disappearing.

| resource | substitution while pending | on failure |
|---|---|---|
| geometry | the shared unit-cube BLAS, instanced with `summary.bounds` folded into the TLAS transform — one BLAS for every placeholder in the scene | skipped, with an issue |
| geometry with no bounds | skipped, which is the honest answer for an adopted mesh that declared none | skipped |
| texture | a 1x1 placeholder seeded from the material's own factor, reached through the bindless slot | magenta, plus an issue |
| material parameter block | rebuilt on demand; it is already rebuilt every epoch, so this is a non-event | — |
| compiled permutation | a neutral fallback hit group, compiled once per cache, reading no per-material block — *landed* | the same fallback |

The texture case is the reason to substitute at the **slot** rather than in `resolve_material`.
Letting a pending texture lose to the next-coarsest frequency would be zero new code — the chain already does that for a missing uv
set — but it flips the *permutation* when the texture lands, so every affected mesh recompiles and restarts its accumulation
mid-load.
Pointing the slot at a placeholder keeps the permutation stable across the whole load.

`bounds` must therefore be available before geometry is, which glTF gives us for free: accessor `min` / `max` is a bounding box
obtained without touching a payload byte.
*Landed*: `sv::mesh_data::bounds` carries it, and `create_mesh` falls back to scanning the positions only when nothing declared one.

## Materials: the definition is permanent, the materialization is not

[material_library](../src/shaped-viewer/material/material_library.hh) is CPU-side and never evicts, and that is exactly what makes
both GPU halves cheap to recover.
The definition is the recipe.

- **The parameter block** (`instance_record`) is a GPU buffer content-keyed on `parameter_key`.
  Its bytes are already rebuilt every epoch, since every bindless index in it is that epoch's, so eviction costs nothing.
- **The compiled permutation** is the slow one.
  Generating HLSL and compiling it is genuinely asynchronous, and it is the resource most likely to be missing when a frame wants to draw.
  It is also the best `bcache` candidate in the design — `ssc::dxc` already has an async content-keyed cache to build on.

A mesh whose permutation has not compiled draws through the fallback hit group: geometry visible, placement correct, neutral
shading, swapping to the real permutation the frame the compile lands.
A prettier fallback — a minimal universal block carrying just `base_color`, evaluated CPU-side from the definition — is a
nice-to-have and explicitly deferred.

This is also why the channel swizzle below needs watching: it belongs in `permutation_key`, and permutations are the thing that
compiles slowly.
Identity swizzles must canonicalize so they hash identically, and the permutation count on a real glTF is worth measuring before
assuming it is fine.

## Asynchronous loading

The stages of a load know different things, and glTF splits exactly where progressive loading needs it to.

1. **Fetch** — uri to bytes, through the resolver, on a worker.
2. **Structure parse** — the glTF JSON chunk alone.
   Cheap, and it yields the whole shape: mesh count, names, materials with all their factors, and bounds from accessor `min` / `max`.
3. **Payload decode** — vertex and index buffers, image decode, tangent-frame generation.
   On a worker, and the expensive part.
4. **Upload** — on the main thread, drained at a bounded rate by the existing `record_pending_work`.

So an asynchronous load is *structurally* complete after stage 2 — the full mesh list, correctly placed, correctly colored from the
material factors, drawn as placeholder boxes — and sharpens per resource as stages 3 and 4 land.

```cpp
if (car.is_ready())                  // structure known; payloads may still be arriving
    for (auto const& m : car.meshes)
        scene.add_mesh(m);
```

OBJ and STL have no structure/payload split, so nothing is known until the whole file is parsed and `is_ready()` simply stays false
longer.
Same API, degenerate behavior, no special case at the call site.

Pending work needs a **priority**: geometry before textures, base color before roughness.
A gray model beats a floating albedo map.

The resource manager is not thread-safe, and does not need to become so: parsing and decoding happen on a worker, and only the
upload touches it.

## Formats

- **glTF 2.0 / GLB** — babel has it.
  Expect gaps: quantized and normalized-integer attributes (babel's typed decode is planned) and sparse accessors (a hard error today).
  Non-triangle primitive modes and `u8` / `u16` indices needing widening are the importer's own work.
- **OBJ** — babel has it, and the importer owes triangulation and vertex dedup.
  `.mtl` is planned in babel and **deferred here**, so an OBJ import carries geometry plus material names only.
- **STL** — *landed* in babel, both containers, and imported as one mesh of raw triangles with no material.
  Its per-facet normals are dropped: the hit shader derives the geometric frame anyway.
- **PLY** — planned in babel, and worth having, though sv has no point-cloud item kind yet.
- **FBX / USD / 3MF / Collada** — out of scope, deliberately rather than as a TODO.
  Each is a project of its own, and the first two mean vendoring something substantial.

## glTF to OpenPBR

OpenPBR is the target type, not `pbr`, and the payoff is immediate: `KHR_materials_transmission`, `_ior`, `_clearcoat` and
`_sheen` map onto `transmission_*`, `coat_*` and `fuzz_*` natively, where `pbr` would discard them.

The core maps directly: `baseColorFactor` to `base_color`, `metallicFactor` to `base_metalness`, `roughnessFactor` to
`specular_roughness`, `normalTexture` to `normal`.

Four decisions the mapping needed:

- **Emission.** `emission_luminance` takes `KHR_materials_emissive_strength` (1 without it) and `emission_color` takes `emissiveFactor`, so strength multiplies luminance.
- **Alpha.** `OPAQUE` binds nothing, leaving the default 1, and `BLEND` binds `opacity` continuously, which the existing `geometry_opacity` path already handles.
  `MASK` needs a new **`alpha_cutoff` attribute on the openpbr type**, defaulting to 0 (disabled), with the fragment stepping opacity against it.
  Baking the step into the imported alpha instead fails as soon as mips filter it, which is exactly when a cutout matters.
- **Occlusion.** `occlusion` is **declared on the type and imported**, because a raster fallback will want baked AO and an asset that loses its AO map on load cannot get it back.
  The path-tracing fragment **ignores it**, with a comment saying why: baked AO in a path tracer is double-counting, since the integrator computes that occlusion correctly and per bounce.
- **Packed textures.** These are the norm rather than the exception, so `texture_sample_source` grows a **channel swizzle**.
  It is four selectors over `{r, g, b, a, zero, one}`, of which the declaration's `component_count()` decides how many are read.
- **Encoded ranges.** A texture rarely stores what the attribute reading it means.
  So `texture_sample_source` also grows a **sample transform**: a per-component scale and bias, applied after the swizzle.
  A tangent-space normal map storing `[0,1]` and meaning `[-1,1]` is that and nothing more, and `normalTexture.scale` folds into the same two numbers.
  `occlusionTexture.strength` is `strength * texel + (1 - strength)`, which is again exactly this.
  Unlike the swizzle it is a VALUE: only whether a transform exists reaches `permutation_key`, and the numbers ride in the parameter block — so a normal scale never forks a permutation.

The swizzle is what makes one metallic-roughness texture bind twice (`base_metalness` from `.b`, `specular_roughness` from `.g`) and
one base color texture bind twice (`base_color` from `.rgb`, `opacity` from `.a`), over a **single upload**, since the content hash
is the same bytes either way.
ORM packing is the same mechanism with three bindings.

What it touches: `resolve_material` carries the swizzle into `resolved_attribute` (which already borrows the sample), the generator
emits `.g` or `.rgb` in the sample expression and widens or narrows to the declaration's format, and `permutation_key` covers it —
it is generated code, not a value.

Color space stays part of `texture_data`'s `sg::pixel_format` and therefore part of its hash.
Base color and emissive are sRGB-encoded while metallic-roughness, normal and occlusion are linear, so a texture bound as both
uploads twice — rare, correct, and better than a per-sample decode flag that makes one resident texture mean two things.

## Tangent frames

The renderer wants a **tangent frame**, not normals: openpbr declares `tangent_frame` (a rotation taking tangent space to object
space) plus `tangent_handedness` (the mirror bit no rotation carries), and `SV_ATTR_SUPPLIED_tangent_frame` is what makes the hit
fall back to the geometric frame when nothing supplied one.
That is half the memory of a normal plus a tangent as vectors.

**Face normals are generated nothing.**
The hit shader computes the geometric frame from the triangle it already has, so importing per-triangle normals would spend memory
and bandwidth to store what is free and could only ever match it.

So the default is to import what the asset carries and generate nothing:

```cpp
enum class sv::frame_generation { none, smooth, crease };

struct sv::tangent_frame_options
{
    bool prefer_file = true;
    sv::frame_generation generate = frame_generation::none;   // none = let the geometric fallback do it
    tg::angle32 crease_angle = 40_deg;
    float weld_epsilon = 0.0f;
};
```

`smooth` is `per_vertex` after a position weld and cannot represent a hard edge.
`crease` is `per_corner`, welded then split by `crease_angle`, which is the one that gives smooth surfaces with hard edges.
`per_corner` is sv's spelling of a half-edge frequency; `per_edge` is reserved and rejected.
Smoothing is a guess about authoring intent, which is why it is asked for rather than assumed.

Because the loader is CPU-side, frame generation is also a **standalone step the caller owns**, not only a load option:

```cpp
sv::generate_tangent_frames(mesh_data, {.generate = frame_generation::crease, .crease_angle = 40_deg});
```

The frame is one attribute, so a caller replacing our algorithm with their own has exactly one thing to produce and no renderer
plumbing to touch.
Those functions are also the ones that eventually move down to `tg::mesh`.

The geometric fallback gives a normal but an arbitrary tangent, so anisotropy and normal maps still want generated, uv-aligned
tangents.
That stays off by default and is enabled per material by the importer when an asset actually carries a normal map.

**TODO, to be written into [TODO.md](TODO.md) when this lands:** our tangents will not match MikkTSpace.
Nearly every DCC tool bakes normal maps against it, so an asset only reproduces exactly if the runtime basis matches the baker's.
Per-corner area- and angle-weighted accumulation is close but not identical, and diverges visibly on high-frequency normal maps and
mirrored uv shells.
Fixing it means porting MikkTSpace's algorithm into the same mesh-processing helpers.

## No filesystem

**Nothing in the importer opens a file.**
The core entry point takes bytes plus a resolver, which is the shape `babel::gltf::read_options::resolve_uri` already has:

```cpp
using sv::uri_resolver = cc::function_ref<cc::result<cc::pinned_data<byte const>>(cc::string_view uri)>;
```

The uri-based convenience is a thin wrapper over a default resolver built on `cc::file_read_stream_adapter`, behind a settable hook
so a host with a virtual filesystem replaces it process-wide rather than threading a resolver through every call.
Relative-uri joining and percent-decoding live in that wrapper, since babel deliberately owns no filesystem policy.

This is explicitly an intermediate: a real virtual filesystem in clean-core supersedes it, and the resolver signature is chosen so
that migration touches one function.
Returning a `pinned_data` also keeps glTF's zero-copy property intact through the importer.

## What this changes in existing code

- `sv::mesh` becomes a bundle of owning handles; today's pinned-bytes mesh becomes `sv::mesh_data`.
- `mesh_texture` carries `texture_data` on the CPU side and a texture handle on the GPU side, which removes the geometry/texture asymmetry.
- `texture_sample_source` gains a channel swizzle, which `resolve_material`, the shader generator and `permutation_key` all have to carry.
- `texture_sample_source` also gains a sample transform, which adds a `material_slot_kind::sample_transform` to the parameter block.
- The openpbr type gains `alpha_cutoff` and `occlusion`; the openpbr fragment steps opacity and ignores occlusion.
- `resolved_material::parameter_key` folds in handle ids rather than content hashes for handle-backed resources, which is the only thing that works for a compute-produced buffer.
- Resource records grow a state, a recipe and a summary; the managers grow the substitution paths and the "adopted resources are not evictable" rule.
- `sv::mesh` carries a CPU-side summary (`cc::optional<tg::aabb3f> bounds`, triangle and vertex counts), because with GPU-only data nothing else can answer a camera-framing question.
- shaped-viewer links `babel-serializer`.

## Phasing

The recipe machinery is the model, not the first milestone.
`from_memory` plus `adopted` covers everything until assets get big, and shipping the importer behind a caching subsystem it does
not yet need would be the wrong order.

1. **The type split.** *Landed*, minus the states: `mesh_data` / `mesh`, symmetric textures, `add_mesh` taking either, examples and tests migrated.
   `sv::mesh` is a bundle of the ids the managers already mint rather than of owning refcounted handles, and a record carries no state or recipe yet —
   the LRU pool keeps owning, and refcounting waits until eviction actually bites.
2. **The material mapping.** *Landed*: `alpha_cutoff`, `occlusion`, and the channel swizzle through resolve, generation and the permutation key.
   What is not yet measured is the open question below — the permutation count on a real glTF, which needs an importer to ask.
3. **The importer, synchronous.** *Landed*: `asset_loader` over glTF, OBJ and STL, through the resolver hook.
   One mesh per (geometry, material), hierarchy flattened, material names namespaced, `prefer_file` tangent frames.
   `babel::stl` landed alongside, reading both containers.
   `texture_sample_source` grew a `sample_transform` alongside the swizzle, which is what makes normal maps and occlusion strength import at all.
   One gap stays: the `KHR_materials_*` extensions are not interpreted by babel, so transmission, ior, clearcoat and sheen do not cross yet.
   `.mtl` stays deferred, as planned.
4. **Placeholders.** *Half landed, half blocked on phase 5.*
   The **fallback hit group** is in: a permutation that has not compiled is substituted for that trace rather than making the view a no-op.
   **Bounds from glTF accessors** are in too — `mesh_data` carries an optional box, and the importer fills it from the accessor's own `min` / `max`.
   The **shared cube BLAS** and the **factor-seeded 1x1 texture** are not, and cannot be yet.
   Every geometry and texture acquire is synchronous, so no resource is ever pending and there is nothing to substitute for.
   Both want the phase below to land first, which argues for folding what is left of this phase into it.
5. **Asynchronous loading.** The four-stage pipeline, structure-first for glTF, prioritized pending work.
6. **Recipes and caching.** `from_uri` and `derived`, `bcache` on the parse and processing steps, re-materialization after eviction, importer versioning in the keys.
7. **Later.** `.mtl`, PLY, MikkTSpace tangents, block compression as a derived recipe, the base-color fallback block.

## Library-extension seams this opens

Per the repo's living-libraries rule, what belongs lower than sv:

- **`tg::mesh` and the mesh-processing helpers.**
  Position welding, crease splitting, tangent-basis construction and orthonormalize-to-quaternion-with-handedness know nothing about rendering.
  They are written here as free functions over spans, so the eventual move down is a file move rather than a rewrite.
  This is also what unblocks babel's own `load_mesh`.
- **`tg::aabb3f` from glTF accessor bounds.**
  Already recorded in babel's [lower-library-gaps.md](../../../data/babel-serializer/docs/lower-library-gaps.md).
  `min_of` / `max_of` hand back untyped spans today, and this design makes the typed box load-bearing.
- **A virtual filesystem in clean-core.** Our resolver hook is a deliberate intermediate for it.
- **An mmap-backed `cc::pinned_data`.**
  The other half of babel's zero-copy story, and what would make a large `.glb` touch its vertex bytes only when read.
- **`.mtl`, `.stl` and `.ply` readers in babel**, which are format work rather than viewer work.

## Open questions

- Whether `viewer::resources()` becomes public.
  Nothing on the simple path needs it — loading needs no manager, and `add_mesh` reaches one through the frame — so this can wait until someone wants explicit GPU meshes outside a frame.
- How the permutation count behaves on a real asset once the swizzle is in the key.
- Whether the fallback hit group is worth a minimal universal `base_color` block, or stays neutral.
