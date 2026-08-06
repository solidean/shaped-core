# Concept: backends

## What a backend is

`sg::context`, `sg::command_list` and `sg::raw_buffer` are **abstract interfaces**.
A *backend* is a self-contained static library under [`backends/`](../../backends/) that subclasses them directly and drives a real graphics API.
`sg::backend::dx12::dx12_context : public sg::context` is the shape; dx12 and vulkan exist today, and a cpu reference, a capture layer or a remote context would be backends too.

The dependency arrow points **one way**: backends depend on `sg`, never the reverse.
The core cannot name a backend, so there is no `sg::create_context(backend_kind)`.
Each backend instead exposes a factory in the `sg` namespace with its **own config type** — `sg::create_dx12_context(dx12_config)` — visible only to a caller that links it.
That is what keeps `sg` from overfitting to today's GPU APIs: an entirely different backend drops in without touching the core.

Backend types are **smurf-named** and namespaced (`sg::backend::dx12::dx12_buffer`), so one backend's symbols are greppable and same-role types across backends never collide.
Backend code is largely public and optimized for **readability over encapsulation** — from `sg`'s side the backend is already opaque, so a second wall inside it buys little.
And backends **share very little code with each other**, the resulting duplication being the deliberate trade.

Each of those is a rule with a reason, and the [coding-guidelines](../coding-guidelines.md) own all of them.
Abstract-interfaces-not-a-bridge, smurf naming, backend-typed create methods, duplicate-rather-than-abstract, the escape hatch.
Read them before working in a backend.

The one part worth restating here is the direction it points.
Genuinely shared code belongs *below* `sg` — the `sg` core, clean-core, typed-geometry — never in a cross-backend layer inside `backends/`.
That is what lets each backend **evolve on its own schedule** as its API demands.

## Backends carry their own tests

Each backend has its **own `*-test` binary** (`shaped-graphics-dx12-test`), built only where that backend builds.
The dx12 suite is gated to Windows by CMake, so it needs no `#ifdef`.
It runs on the **WARP** software adapter, present on any Windows host, so the whole suite also runs on headless CI.
A shared `make_warp_context()` in [`dx12-test-common.hh`](../../backends/dx12/tests/dx12-test-common.hh) hands one back.

Two kinds of test belong in a backend suite.
**Feature smoke tests** — one end-to-end exercise per feature against a live device — and **backend-internal invariant tests** for behaviour invisible through the abstract surface.
Which test goes in which tier, and the rules for driving one, are [testing](../testing.md)'s job.

Tests are split **per topic**, one `.cc` per area, so the suite stays navigable as it grows:

```text
backends/dx12/tests/
  main.cc                     # nx::run entry point
  dx12-test-common.hh         # shared make_warp_context()
  dx12-context-test.cc        # context / command-list / buffer bring-up
  dx12-epoch-test.cc          # epoch advance/retire, deferred deletion, submission token
  dx12-command-pool-test.cc   # allocator + command-list pooling (backend-internal invariants)
  dx12-transfer-test.cc       # inline upload / download
  dx12-copy-test.cc           # device→device buffer copy
```

## What backend tests are *not* for

A backend suite is **not** where public-API semantics are nailed down.
The backend-agnostic `shaped-graphics-test` exercises **every** backend uniformly through the abstract `sg` types, validating the public contract once.
A semantics test that should hold for dx12 *and* vulkan *and* a future cpu backend belongs there, never duplicated per backend.
Keep the per-backend suites to smoke coverage and internal invariants.

## See also

- [coding-guidelines](../coding-guidelines.md) — the concrete backend rules this narrates.
- [epochs](epochs.md) — the lifetime/synchronization model backend invariant tests lean on.
- [inline upload](upload.inline.md) / [inline download](download.inline.md) — backend features with their own smoke tests.
