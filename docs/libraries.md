# Libraries

The catalog of shaped-core libraries — an expanded version of the table in the
[README](../README.md). The set is **growing**; this list is current, not
exhaustive.

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

## Dependency graph

```text
nexus
  ↓
clean-core
  ↓
(no dependencies)
```

For the build & test workflow shared by all libraries, see
[guides/building-and-testing.md](guides/building-and-testing.md).
