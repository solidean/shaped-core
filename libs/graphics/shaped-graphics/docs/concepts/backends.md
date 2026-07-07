# Concept: backends

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full rules (those are the [coding-guidelines](../coding-guidelines.md)). See also
> [epochs](epochs.md), [threading](threading.md), [inline upload](upload.inline.md).

## What a backend is

`sg::context`, `sg::command_list`, and `sg::raw_buffer` are **abstract interfaces**. A *backend* is a
self-contained static library under [`backends/`](../../backends/) that subclasses them directly
(`sg::backend::dx12::dx12_context : public sg::context`) and drives a real graphics API — dx12,
vulkan, and later others (a cpu reference, a capture/debug layer, a remote context).

The dependency arrow points **one way**: backends depend on `sg`, never the reverse. The core cannot
name a backend, so there is no `sg::create_context(backend_kind)`. Instead each backend exposes a
factory in the `sg` namespace with its **own config type** — `sg::create_dx12_context(dx12_config)` —
and only a caller that links that backend sees it. This is what keeps `sg` from overfitting to
today's GPU APIs: an entirely different backend drops in without touching the core.

Backend types are **smurf-named** and namespaced (`sg::backend::dx12::dx12_buffer`) so one backend's
symbols are greppable and same-role types across backends never collide. Backend code is largely
public and optimized for **readability over encapsulation** — from `sg`'s perspective the backend is
already opaque (it can't depend on it), so a second wall inside the backend buys little.

The precise rules behind all of the above live in the
[coding-guidelines](../coding-guidelines.md) (the backend bridge, smurf naming, backend-typed create
methods, the escape hatch). Read those before working in a backend.

## Why we duplicate across backends

We deliberately **share very little code between backends** and treat the resulting duplication as
fine: when dx12 and vulkan need similar-looking logic, we write it twice rather than hoisting a shared
cross-backend helper. Cross-backend abstraction layers leak — they accumulate conditionals for each
API's quirks and make every backend harder to read. Independent per-backend code, even duplicated, is
easier to understand and lets each backend **evolve on its own schedule** as the underlying API
demands. Genuinely shared code belongs *below* `sg` (in the `sg` core, clean-core, or typed-geometry),
never in a cross-backend layer inside `backends/`.

## Backends carry their own tests

Each backend has its **own `*-test` binary** (`shaped-graphics-dx12-test`), built only where that
backend builds — the dx12 suite is gated to Windows by CMake, so it needs no `#ifdef`. It runs on the
**WARP** software adapter, which is present on any Windows host, so the whole suite also runs on
headless CI (a shared `make_warp_context()` helper in
[`dx12-test-common.hh`](../../backends/dx12/tests/dx12-test-common.hh) hands one back).

Two kinds of test belong in a backend suite:

1. **Feature smoke tests** — one straightforward end-to-end exercise per feature (create a context,
   round-trip a buffer upload/download, copy buffer-to-buffer). They confirm the backend's own path
   works against a live device; they are *not* trying to pin down the full public-API semantics.
2. **Backend-internal invariant tests** — validate behaviour that only exists inside the backend and
   is invisible through the abstract `sg` surface: command-list / allocator **pooling**, epoch-gated
   recycling, ring-buffer reclaim, future caching. These `static_cast` the `sg::context` handle to the
   concrete `dx12_context` and inspect its guts (`c._cmd_pool.free_allocator_count(...)`), which is
   exactly the "here be dragons" escape hatch — legitimate *inside a backend's own test*, where the
   test is deliberately coupled to that backend.

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

Backend suites are **not** where we nail down public-API semantics. The backend-agnostic
`shaped-graphics-test` will grow an extensive suite that exercises **every** backend uniformly through
the abstract `sg` types and validates the public contract once — independent of any one backend. A
semantics test that should hold for dx12 *and* vulkan *and* a future cpu backend belongs there, not
duplicated per backend. Keep the per-backend suites to smoke coverage and internal invariants; push
anything that is really a statement about the `sg` API up into the shared suite.

## See also

- [coding-guidelines](../coding-guidelines.md) — the concrete backend rules this narrates.
- [epochs](epochs.md) — the lifetime/synchronization model backend invariant tests lean on.
- [inline upload](upload.inline.md) / [inline download](download.inline.md) — backend features with
  their own smoke tests.
