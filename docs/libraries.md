# Libraries

The catalog of shaped-core libraries, an expanded version of the table in the [README](../README.md).
Both the set of libraries and each library's scope are still **growing**.
So the entries below describe what each library is *for* — its intended scope, and therefore what belongs where — even where the implementation is still being filled in.
Per-library roadmaps track what exists today.

Libraries live under `libs/<category>/<lib>`: `src/<lib>/` with colocated `.hh`/`.cc`, `tests/` holding a `<lib>-test` binary, and an optional `docs/`.
A library depends only on lower libraries — there are **no upward or cyclic dependencies** — and each entry below names what it depends on.
nexus is the one exception, and the reason is under its entry: as the harness nothing links, it is a leaf rather than a base layer.
The `CMakeLists.txt` files are the ground truth if an entry and the build ever disagree.

## base

The foundational layer everything else builds on.

### clean-core — namespace `cc` — no dependencies

[readme](../libs/base/clean-core/readme.md) · [docs](../libs/base/clean-core/docs/_index.md)

Foundational C++23 building blocks: data structures, memory utilities, assertions and low-level primitives.
Highlights:

- **Containers & views** — `vector`, `array` (and the `fixed_` / `unique_` variants), `map`, `set`, `ringbuffer`, `bitset`, `disjoint_set`,
  `pair` / `tuple` / `variant`, plus non-owning `span` / `strided_span`.
- **Strings** — owning `string` (with SSO), `string_view`, `char_predicates`, and `to_string` / `to_debug_string`.
  Plus `cc::format`, a `std::format`-style formatter whose format strings are validated at compile time.
- **Fallible values** — `optional` and `result<T, E>` for expected-error handling.
- **Callables** — non-owning `function_ref`, move-only `unique_function`.
- **Memory** — `allocation` / `node_allocation` handles over `memory_resource`s, plus `shared_ptr`, an 8 B intrusive handle whose Traits protocol is still provisional.
- **Foundations** — a lean assertion suite (`CC_ASSERT` …), compiler/OS macros,
  bit utilities, `mutex`, and the lazy `sequence` ranges API.

The source tree is organized by topic, and the [readme](../libs/base/clean-core/readme.md#file-organization) has the map.

### nexus — namespace `nx` — depends on clean-core, babel-data (private)

[readme](../libs/base/nexus/readme.md) · [docs](../libs/base/nexus/docs/_index.md)

Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML), so IDE test integration works out of the box.
This is what every `<lib>-test` binary is built on.
Beyond `TEST` / `CHECK`, it carries invocable (parametrized) tests, an API-sequence fuzzer, guide benchmarks, and hardware performance counters.

**nexus is a leaf, not a base layer.**
Nothing in shaped-core links it except test binaries, so the harness sits on top of the libraries it tests even though it lives in `base/`, and it is added last in the root `CMakeLists.txt`.
That is what lets it write its JSON sidecars — the test listing and the perf metrics — through `babel::json` instead of hand-rolling an escaper.
It links `babel-data` rather than all of babel, and that constraint belongs to nexus rather than to babel: every `*-test` binary links nexus, so whatever nexus depends on is a tax the whole tree pays.
The source tree is organized by responsibility, and the [readme](../libs/base/nexus/readme.md#file-organization) has the map.

### typed-geometry — namespace `tg` — depends on clean-core

[readme](../libs/base/typed-geometry/readme.md) · [docs](../libs/base/typed-geometry/docs/_index.md)

The repo's strongly-typed math and geometry vocabulary.
The type system encodes the geometry, so `vec`, `pos`, `comp` and the oriented `bivec` are distinct types with distinct algebra rather than interchangeable tuples.
This is the intended home for anything mathematical or geometric in shaped-core.
If a task wants a new shape, transform, query, curve, color space, sampler, acceleration structure or exact-number type, it almost certainly belongs here.
The planned scope:

- **scalar** — a `scalar_traits<T>` seam so custom scalars slot in (exact/symbolic
  numbers, intervals, autodiff), plus `angle<T>` and constants.
- **linalg** — `vec` / `pos` / `comp` with affine rules, oriented `bivec` (+
  `cross`/`dual`/`undual`), column-major `mat`, and `quat`.
- **transform** — semantic transform types (rigid / similarity / affine /
  projective), kept distinct from raw `mat` data.
- **geometry** — primitives (`aabb`, `triangle`, `segment`, `ray`, `line`,
  `plane`, and onward to spheres, frusta, polygons, …) classified by an
  `object_traits` set-of-points seam (`intrinsic_dim` / `ambient_dim` /
  `is_finite`), plus the queries over them (distance, closest points,
  intersection, containment).
- **curves, color, sampling, spatial** (bvh / kd-tree / grid), **symbolic CAS,
  calculus, and mesh** data structures & algorithms.

[structure.md](../libs/base/typed-geometry/docs/structure.md) carries both what exists today — the scalar seam, the linalg core, the first geometry primitives — and the full roadmap.

## data

Understanding data: the formats it arrives in, and the structures that outlive a session — documents, their history, and the files they live in.
Layered on `base`, below the graphics stack.

### babel-serializer — namespace `babel` — depends on clean-core, typed-geometry

[readme](../libs/data/babel-serializer/readme.md) ·
[cheat-sheet](../libs/data/babel-serializer/cheat-sheet.md) ·
[docs](../libs/data/babel-serializer/docs/_index.md)

Serialization and deserialization of various formats, often thin wrappers over existing libraries and often our own take on a parse.
Two layers, kept distinct: each format parses into an **unopinionated native structure**, and **opinionated aggregators** — "load an image", "load a mesh" — sit on top of those.
Reading is optimized for the read-once case and takes a `cc::read_stream`, with one deviation for a format that must hand back zero-copy views of its input.
[coding-guidelines.md](../libs/data/babel-serializer/docs/coding-guidelines.md) owns all of that.
Today: a base64 codec, JSON reading and writing, a markdown reader plus a SQLite engine wrapper (`data/`), and Wavefront OBJ and glTF 2.0/GLB readers (`geometry/`).
Plus PNG/JPEG read+write, with the `babel::image` aggregator on top (`image/`).

**One namespace, several link targets.**
`babel-data` is the externals-free base — base64, JSON and markdown over clean-core alone — and `babel-serializer` is everything else on top of it, the target to link when in doubt.
The split is by dependency rather than by topic, so `data/` itself spans both: `sqlite` sits in the upper target because of what it needs, not because of what it is.

The roadmap lives in [structure.md](../libs/data/babel-serializer/docs/structure.md).

### versioned-document — namespace `vdoc` — depends on clean-core

[readme](../libs/data/versioned-document/readme.md) ·
[cheat-sheet](../libs/data/versioned-document/cheat-sheet.md) ·
[docs](../libs/data/versioned-document/docs/_index.md)

Structured documents that are versioned, mergeable and verifiable.
A document is entities holding components holding properties — but the source of truth is not that document, it is an immutable content-addressed DAG of **ops** that everything is materialized from.

The library **ships zero components**: what a `transform` or a `material` is belongs to the application.
`vdoc` owns only the machinery that gives every application's components the same guarantees.
Four layers, kept strictly apart: the op DAG, the schema-agnostic raw document, the typed immutable index, and — one library up — persistence.

Two properties shape everything else.
Property values are a **canonical binary codec where equality is byte equality**, which is what makes diffing, content addressing and merge decisions memcmps.
The typed document is **immutable**: edits build an op and re-materialize, so a parsed document is safe to hold across threads for as long as it is useful.

Complete, both libraries.
[docs/](../libs/data/versioned-document/docs/_index.md#concepts) is the design, one file per concept.
[decisions.md](../libs/data/versioned-document/docs/decisions.md) records every settled choice and what would reopen it.

### versioned-document-file — namespace `vdoc::file` — depends on versioned-document, babel-serializer

[readme](../libs/data/versioned-document-file/readme.md) ·
[cheat-sheet](../libs/data/versioned-document-file/cheat-sheet.md) ·
[docs](../libs/data/versioned-document-file/docs/_index.md)

The `.vdoc` save format: one SQLite file holding a document's whole history, the assets a user embedded in it, and the UI state that goes with it.
babel-serializer is linked privately, so no sqlite type reaches a public header.

A `.vdoc` file holds three kinds of state, and **only one of them is immutable**.
History is content-addressed and verified, blobs are content-addressed but their asset names are not, and the workspace is disposable.
So op ids do not commit to asset content, deliberately: without that escape hatch, replacing a placeholder asset would be a rewrite of history.
[format.md](../libs/data/versioned-document-file/docs/format.md) specifies the bytes and says why at length.

### blob-cache — namespace `bcache` — depends on clean-core, babel-serializer

[readme](../libs/data/blob-cache/readme.md) ·
[cheat-sheet](../libs/data/blob-cache/cheat-sheet.md) ·
[docs](../libs/data/blob-cache/docs/_index.md)

A persistent cache for results that are expensive to produce and cheap to recognize: geometry processing output,
serialized acceleration structures, generated archives, downloaded artifacts.
One SQLite file, shared by every process on the machine that opens the same path.
babel-serializer is linked privately, so no sqlite type reaches a public header.

Everything about it follows from one invariant: **deleting all cache data can never affect correctness.**
That is what makes a storage failure a miss rather than an error, an incompatible file something to discard rather
than migrate, and cross-process duplicate computation a cost rather than a bug.

Entries are keyed `(namespace, key, version)`, immutable, and first-writer-wins; the bytes behind them are
content-addressed, so two entries with identical payloads name one object.
`acquire` singleflights the whole pipeline — lookup, compute, store — rather than only the compute, and eviction
weighs recompute cost per byte of disk rather than age alone.
[design.md](../libs/data/blob-cache/docs/design.md) is the reasoning, decision by decision.

## graphics

The graphics stack, layered on top of `base`.
[graphics.md](graphics.md) is the family overview; each library's own docs carry its current state.

### shaped-graphics — namespace `sg` — depends on clean-core, typed-geometry

[readme](../libs/graphics/shaped-graphics/readme.md) · [docs](../libs/graphics/shaped-graphics/docs/_index.md)

The graphics-API wrapper: a small, backend-agnostic surface — `context`, `command_list`, and the GPU resource types — over concrete graphics backends.
It also owns the **render-routine framework** (`sg::render_routine` plus the per-context `ctx.routines` registry), whose concrete routines live in shaped-rendering.
Backends are **separate static libraries**, smurf-named and namespaced (`sg::backend::dx12::dx12_context`), one per API.
dx12 and vulkan are the two that exist — dx12 is real, vulkan stubs its recording paths — with metal/webgpu and opengl/webgl intended but unwritten.
Resources are shared-immutable, handed out as `xyz_handle` — a `std::shared_ptr<sg::xyz const>` for a resource, without the `const` for the mutable `context` and `swapchain`.
There are no host-visible resources: PCIe transfer is a globally shared resource sg manages.
How a backend derives from those interfaces is [backends.md](../libs/graphics/shaped-graphics/docs/concepts/backends.md)'s.

### shaped-shader-compiler-dxc — namespace `ssc::dxc` — depends on shaped-graphics

[readme](../libs/graphics/shaped-shader-compiler-dxc/readme.md) ·
[cheat-sheet](../libs/graphics/shaped-shader-compiler-dxc/cheat-sheet.md)

A side utility rather than part of the sv → sr → sg chain.
It is a lean wrapper over the DirectX Shader Compiler that turns HLSL into an `sg::compiled_shader`: bytecode, reflected bindings, compute workgroup size.
Two-step: `preprocess` resolves `#include`s through a caller-supplied resolver, with no file I/O baked in, then `compile`.
On top of that sits an async, content-keyed `shader_cache`.
**Windows-only**, and built only once DXC has been fetched — `extern/dxc` downloads a pinned release on demand.

### shaped-shader-library — namespace `slib` — depends on shaped-graphics, and on shaped-shader-compiler-dxc where DXC exists

[readme](../libs/graphics/shaped-shader-library/readme.md) ·
[cheat-sheet](../libs/graphics/shaped-shader-library/cheat-sheet.md) ·
[docs](../libs/graphics/shaped-shader-library/docs/_index.md)

The shader package + hot-reload mechanism, also a side utility.
Any target — a library, an app, or a test binary — declares its own **shader package** in its own CMakeLists (`sc_add_shader_package`), gets typed C++ symbols for its shaders, and gets hot reloading.
sg itself does not depend on it.
You `acquire(ctx)` with the context you will use the shader on and get bytecode in a format *it* accepts, so one authored shader can feed several backends.
Compilers are a registered seam (HLSL→DXIL today), and shader sources are reached only through a mountable virtual filesystem.

**How the shader system fits together — [shaped-graphics/docs/shaders.md](../libs/graphics/shaped-graphics/docs/shaders.md).**

### shaped-rendering — namespace `sr` — depends on shaped-graphics, shaped-shader-library

[readme](../libs/graphics/shaped-rendering/readme.md) · [docs](../libs/graphics/shaped-rendering/docs/_index.md)

Render routines and helpers on top of sg: the reusable building blocks of a renderer — mipmap generation, texture compression, tonemapping.
Of those `sr::blit_routine` exists today, alongside the Dear ImGui renderer and the SDL3-backed window abstraction.
Concrete routines acquire their shaders through shaped-shader-library, which is why sr depends on it, and it also links the vendored Dear ImGui + ImPlot + ImGuizmo bundle.
Home to the **Dear ImGui renderer** (`sr::imgui_context` + `sr::imgui_routine`), drawn entirely through sg — see [imgui.md](../libs/graphics/shaped-rendering/docs/imgui.md).
Also home to the **window abstraction** (`sr::window_system` / `sr::window`), backed by SDL3 and exposing none of it, whose native handle feeds `sg::swapchain_description`.
The API is always present: without a backend, because SDL3 was not fetched, `window_system::try_create` fails rather than the types disappearing, and `SR_HAS_WINDOW` says whether one was compiled in.

### shaped-viewer — namespace `sv` — depends on shaped-rendering

[readme](../libs/graphics/shaped-viewer/readme.md) · [docs](../libs/graphics/shaped-viewer/docs/_index.md)

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API, serving Shaped Code's visualization needs.
The top of the graphics stack, with a first vertical slice: path-traced views blitted into a window.

For the build and test workflow shared by all libraries, see [guides/building-and-testing.md](guides/building-and-testing.md).
