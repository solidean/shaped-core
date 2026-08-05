# Libraries

The catalog of shaped-core libraries, an expanded version of the table in the [README](../README.md).
Both the set of libraries and each library's scope are still **growing**.
So the entries below describe what each library is *for* — its intended scope, and therefore what belongs where — even where the implementation is still being filled in.
Per-library roadmaps track what exists today.

Libraries live under `libs/<category>/<lib>`: `src/<lib>/` with colocated `.hh`/`.cc`, `tests/` holding a `<lib>-test` binary, and an optional `docs/`.
A library depends only on lower libraries — there are **no upward or cyclic dependencies** — and each entry below names what it depends on.
The `CMakeLists.txt` files are the ground truth if an entry and the build ever disagree.

## base

The foundational layer everything else builds on.

### clean-core — namespace `cc` — no dependencies

[readme](../libs/base/clean-core/readme.md) · [docs](../libs/base/clean-core/docs/_index.md)

Foundational C++23 building blocks: data structures, memory utilities, assertions and low-level primitives.
Highlights:

- **Containers & views** — `vector`, `array` (and the `fixed_` / `unique_` variants), `map`, `set`, `ringbuffer`, `bitset`, `disjoint_set`,
  `pair` / `tuple` / `variant`, plus non-owning `span` / `strided_span`.
- **Strings** — owning `string` (with SSO), `string_view`, `char_predicates`, and
  `to_string` / `to_debug_string`.
- **Fallible values** — `optional` and `result<T, E>` for expected-error handling.
- **Callables** — non-owning `function_ref`, move-only `unique_function`.
- **Memory** — `allocation` / `node_allocation` handles over `memory_resource`s, plus `shared_ptr`, an 8 B intrusive handle whose Traits protocol is still provisional.
- **Foundations** — a lean assertion suite (`CC_ASSERT` …), compiler/OS macros,
  bit utilities, `mutex`, and the lazy `sequence` ranges API.

The source tree is organized by topic, and the [readme](../libs/base/clean-core/readme.md#file-organization) has the map.

### nexus — namespace `nx` — depends on clean-core

[readme](../libs/base/nexus/readme.md) · [docs](../libs/base/nexus/docs/_index.md)

Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML), so IDE test integration works out of the box.
This is what every `<lib>-test` binary is built on.
Beyond `TEST` / `CHECK`, it carries invocable (parametrized) tests, an API-sequence fuzzer, guide benchmarks, and hardware performance counters.
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

## io

Getting data in and out, in various formats.
Layered on `base`, below the graphics stack.

### babel-serializer — namespace `babel` — depends on clean-core, typed-geometry

[readme](../libs/io/babel-serializer/readme.md) ·
[cheat-sheet](../libs/io/babel-serializer/cheat-sheet.md) ·
[docs](../libs/io/babel-serializer/docs/_index.md)

Serialization and deserialization of various formats, often thin wrappers over existing libraries and often our own take on a parse.
Two layers, kept distinct.
Each format parses into an **unopinionated native structure**: JSON stays a tree of values, OBJ stays parallel attribute arrays.
**Opinionated aggregators** — "load an image" or "load a mesh" across formats — sit on top of those.
Reading is optimized for the read-once-into-a-basically-immutable-structure case: great to traverse and query, deliberately not for insertion.
Writing gets a separate API.
Readers take a `cc::read_stream` and parse against its buffered window rather than slurping the input.
The one deviation is a format whose result must hand back zero-copy views *of* its input, which takes a `cc::pinned_data<byte const>` instead.
Today: a base64 codec, JSON and markdown readers plus a SQLite engine wrapper (`data/`), and Wavefront OBJ and glTF 2.0/GLB readers (`geometry/`).
Plus PNG/JPEG read+write, with the `babel::image` aggregator on top (`image/`).
The roadmap lives in [structure.md](../libs/io/babel-serializer/docs/structure.md).

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

Render routines and helpers on top of sg: the reusable building blocks of a renderer — mipmap generation, texture compression, tonemapping — still an early-stage skeleton.
Concrete routines acquire their shaders through shaped-shader-library, which is why sr depends on it, and it also links the vendored Dear ImGui + ImPlot + ImGuizmo bundle.
Home to the **Dear ImGui renderer** (`sr::imgui_context` + `sr::imgui_routine`), drawn entirely through sg — see [imgui.md](../libs/graphics/shaped-rendering/docs/imgui.md).
Also home to the **window abstraction** (`sr::window_system` / `sr::window`), backed by SDL3 and exposing none of it, whose native handle feeds `sg::swapchain_description`.
The API is always present: without a backend, because SDL3 was not fetched, `window_system::try_create` fails rather than the types disappearing, and `SR_HAS_WINDOW` says whether one was compiled in.

### shaped-viewer — namespace `sv` — depends on shaped-rendering

[readme](../libs/graphics/shaped-viewer/readme.md) · [docs](../libs/graphics/shaped-viewer/docs/_index.md)

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API, serving Shaped Code's visualization needs.
The top of the graphics stack, and an early-stage skeleton.

For the build and test workflow shared by all libraries, see [guides/building-and-testing.md](guides/building-and-testing.md).
