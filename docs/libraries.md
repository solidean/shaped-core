# Libraries

The catalog of shaped-core libraries — an expanded version of the table in the
[README](../README.md). Both the set of libraries and each library's scope are
still **growing**: the entries below describe what each library is *for* — its
intended scope — so it's clear what belongs where, even where the implementation
is still being filled in. Per-library roadmaps track what exists today.

Libraries live under `libs/<category>/<lib>`. Each is `src/<lib>/` (colocated
`.hh`/`.cc`), `tests/` (a `<lib>-test` binary), and an optional `docs/`. A library
depends only on lower libraries — there are **no upward or cyclic dependencies**,
so the stack reads bottom-up.

## base

The foundational layer everything else builds on.

### clean-core — namespace `cc` — no dependencies

[readme](../libs/base/clean-core/readme.md) · [docs](../libs/base/clean-core/docs/_index.md)

Foundational C++23 building blocks: data structures, memory utilities, assertions,
and low-level primitives. Highlights:

- **Containers & views** — `vector`, `array` (and `fixed_`/`unique_` variants),
  `map`, `set`, `ringbuffer`, `bitset`, `disjoint_set`, `pair`/`tuple`/`variant`,
  plus non-owning `span` / `strided_span`.
- **Strings** — owning `string` (with SSO), `string_view`, `char_predicates`, and
  `to_string` / `to_debug_string`.
- **Fallible values** — `optional` and `result<T, E>` for expected-error handling.
- **Callables** — non-owning `function_ref`, move-only `unique_function`.
- **Memory** — `allocation` / `node_allocation` handles over `memory_resource`s.
- **Foundations** — a lean assertion suite (`CC_ASSERT` …), compiler/OS macros,
  bit utilities, `mutex`, and the lazy `sequence` ranges API.

The source tree is organized by topic — see the
[readme](../libs/base/clean-core/readme.md#file-organization) for the map.

### nexus — namespace `nx` — depends on clean-core

[docs](../libs/base/nexus/docs/catch2-runner-compat.md)

Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering,
sections, JUnit XML), so IDE test integration works out of the box. This is what
every `<lib>-test` binary is built on.

### typed-geometry — namespace `tg` — depends on clean-core

[readme](../libs/base/typed-geometry/readme.md) · [docs](../libs/base/typed-geometry/docs/_index.md)

The repo's strongly-typed math & geometry vocabulary: the type system encodes the
geometry, so `vec`, `pos`, `comp`, and the oriented `bivec` are distinct types
with distinct algebra rather than interchangeable tuples. This is the intended
home for anything mathematical or geometric in shaped-core — if a task wants a new
shape, transform, query, curve, color space, sampler, acceleration structure, or
exact-number type, it almost certainly belongs here. The planned scope:

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

What exists today (the scalar seam, the linalg core, and the first geometry
primitives) and the full roadmap live in
[structure.md](../libs/base/typed-geometry/docs/structure.md).

## graphics

The graphics stack, layered on top of `base`. See [graphics.md](graphics.md) for the family
overview. Early-stage: docs + buildable skeletons, with sg's core types and dx12/vulkan backends
currently stubbed.

### shaped-graphics — namespace `sg` — depends on clean-core, typed-geometry

[readme](../libs/graphics/shaped-graphics/readme.md) · [docs](../libs/graphics/shaped-graphics/docs/_index.md)

The graphics-API wrapper: a small, backend-agnostic surface — `context`, `command_list`, and
GPU resource types (`buffer` today; `texture`, `pipeline`, … to come) — over concrete graphics
backends. Backends are **separate static libraries**, smurf-named and namespaced
(`sg::backend::dx12::dx12_context`), one per API: dx12/vulkan (tier 1), metal/webgpu (tier 2),
opengl/webgl (legacy). The public `context`/`command_list`/`buffer` are abstract interfaces that
each backend subclasses directly (no separate bridge layer); cheap shared metadata lives in the
base as protected members. Resources are shared-immutable and handed out as `xyz_handle`
(`= std::shared_ptr<sg::xyz>`); there are no host-visible resources — PCIe transfer is a globally
shared resource sg manages.

### shaped-shader-compiler-dxc — namespace `ssc::dxc` — depends on shaped-graphics

[readme](../libs/graphics/shaped-shader-compiler-dxc/readme.md) ·
[cheat-sheet](../libs/graphics/shaped-shader-compiler-dxc/cheat-sheet.md)

A side utility, not part of the sv→sr→sg chain: a lean wrapper over the DirectX Shader Compiler that
turns HLSL into an `sg::compiled_shader` (bytecode + reflected bindings + compute workgroup size).
Two-step — `preprocess` (resolve `#include`s through a caller-supplied resolver, no file I/O baked in)
then `compile` — plus an async, content-keyed `shader_cache`. **Windows-only**, and built only once
DXC has been fetched (`extern/dxc` downloads a pinned release on demand).

### shaped-shader-library — namespace `slib` — depends on shaped-graphics

[readme](../libs/graphics/shaped-shader-library/readme.md) ·
[cheat-sheet](../libs/graphics/shaped-shader-library/cheat-sheet.md) ·
[docs](../libs/graphics/shaped-shader-library/docs/_index.md)

The shader package + hot-reload mechanism, also a side utility. Any target — a library, an app, or a
test binary — declares its own **shader package** in its own CMakeLists (`sc_add_shader_package`), gets
typed C++ symbols for its shaders, and gets hot reloading; sg itself does not depend on it. You
`acquire(ctx)` with the context you will use the shader on, and get bytecode in a format *it* accepts —
so one authored shader can feed several backends. Compilers are a registered seam (HLSL→DXIL today),
shader sources are reached only through a mountable virtual filesystem, and the generator embeds every
source so a shipped binary is self-contained without a mode flag.

**How the shader system fits together —
[shaped-graphics/docs/shaders.md](../libs/graphics/shaped-graphics/docs/shaders.md).**

### shaped-rendering — namespace `sr` — depends on shaped-graphics

[readme](../libs/graphics/shaped-rendering/readme.md) · [docs](../libs/graphics/shaped-rendering/docs/_index.md)

Render routines and helpers on top of sg — the reusable building blocks of a renderer (mipmap
generation, texture compression, tonemapping, …), still an early-stage skeleton.
Also home to the **window abstraction** (`sr::window_system` / `sr::window`), backed by SDL3 and exposing none of it.
A window's native handle feeds `sg::swapchain_description`.
The API is always present; without a backend (SDL3 not fetched) `window_system::try_create` fails rather
than the types disappearing, and `SR_HAS_WINDOW` says whether one was compiled in.

### shaped-viewer — namespace `sv` — depends on shaped-rendering

[readme](../libs/graphics/shaped-viewer/readme.md) · [docs](../libs/graphics/shaped-viewer/docs/_index.md)

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API,
serving Shaped Code's visualization needs. The top of the graphics stack. Early-stage skeleton.

## Dependency graph

```text
shaped-viewer              shaped-shader-library      # the shader side utilities: they depend
     ↓                              ↓                 # on sg, nothing depends on them
shaped-rendering           shaped-shader-compiler-dxc
     ↓                              ↓
     └──────────→ shaped-graphics ←─┘ ──→ backends (dx12, vulkan, metal, webgpu, opengl, webgl)
                        ↓
              typed-geometry     nexus
                     ↓             ↓
                     └─ clean-core ┘
                           ↓
                     (no dependencies)
```

For the build & test workflow shared by all libraries, see
[guides/building-and-testing.md](guides/building-and-testing.md).
