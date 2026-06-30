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

## Dependency graph

```text
nexus    typed-geometry
   ↓         ↓
     clean-core
        ↓
  (no dependencies)
```

For the build & test workflow shared by all libraries, see
[guides/building-and-testing.md](guides/building-and-testing.md).
