# Testing shaped-graphics

sg is strongly test-driven, and its tests are split into **two tiers** by what they pin down. Getting a
new test into the right tier keeps the public contract validated *once* across all backends, and keeps
backend-specific machinery tested where it actually lives.

```text
libs/graphics/shaped-graphics/
  tests/                         # tier 1: backend-agnostic API tests  ->  shaped-graphics-test
  backends/<backend>/tests/      # tier 2: per-backend suites          ->  shaped-graphics-<backend>-test
```

Run everything through `dev.py` — never a `*-test` binary directly:

```bash
uv run dev.py test "sg - transient buffer round-trips within its epoch"   # one API test, every backend
uv run dev.py test "sg dx12 -"                                            # the dx12 backend suite
uv run dev.py test                                                        # the full repo suite
```

---

## Tier 1 — backend-agnostic API tests (`tests/`)

This is the primary suite and **the default home for a new test.** It validates the public `sg` contract
through the abstract types (`sg::context`, `buffer`, `command_list`, …), independent of any one backend,
and runs that single test body against **every backend the platform has and that is mature enough**.

The mechanism is nexus `INVOCABLE_TEST`: a test that takes a live `sg::context_handle` parameter.

```cpp
INVOCABLE_TEST("sg - transient buffer round-trips within its epoch", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);
    // ... drive ctx-> through the public API only ...
}
```

It becomes runnable against each backend by two pieces working together:

- **Entry drivers** — [`tests/backends/<backend>-entry.cc`](../tests/backends/) create a concrete context
  (dx12 on WARP, …) and `nx::invoke_tests("<backend>", ctx)` every invocable against it. A backend that
  can't come up (no device) `SKIP`s; a backend that isn't mature yet stays **unregistered** so it is
  neither swept nor aliased (see [`vulkan-entry.cc`](../tests/backends/vulkan-entry.cc)).
- **Alias setup** — [`tests/backends/backends.cc`](../tests/backends/backends.cc) defines, for each
  invocable, an alias of the same name expanding to one scoped run per registered backend. So
  `dev.py test "sg - <name>"` runs it on dx12, vulkan, … — whichever backends this binary was built with.

Full mechanism: [nexus/docs/invocable-tests.md](../../../base/nexus/docs/invocable-tests.md).

**What belongs here:** every statement about the public API — allocation shapes, lifetime/epoch semantics,
transfer round-trips, binding validation, the transient budget contract. Anything that should hold for
dx12 *and* vulkan *and* a future cpu backend goes here, written once, not duplicated per backend. Complex
and edge-case coverage lives here too; prefer this tier and only drop to tier 2 when you genuinely need
backend internals or a backend-specific resource (e.g. an embedded shader blob).

Tests are split **per topic**, one `.cc` per area (`buffer/`, `transfer/`, `binding/`, `transient/`, …),
and each topic file is added to the `if(_sg_backends)` block in the library
[`CMakeLists.txt`](../CMakeLists.txt) (agnostic tests need at least one backend to run against).

> A backend-agnostic test still needs *some* backend to execute. Until a always-available CPU/validation
> backend exists (a documented TODO in `CMakeLists.txt`), tier 1 runs only where a real backend builds —
> today that means dx12 on Windows.

---

## Tier 2 — per-backend suites (`backends/<backend>/tests/`)

Each backend has its **own `*-test` binary**, built only where that backend builds, running on a software
adapter where possible (dx12 → WARP) so it also runs on headless CI. Two kinds of test belong here:

1. **Feature smoke tests** — one straightforward end-to-end exercise per feature, confirming the backend's
   own path works against a live device (not restating the full public semantics — tier 1 does that).
2. **Backend-internal invariants** — behaviour invisible through the abstract surface: descriptor-ring and
   ring-buffer reclaim, bump-allocator placement granularity, command-list/allocator pooling, epoch-gated
   recycling. These `static_cast` the handle to the concrete context and inspect its guts — the legitimate
   "here be dragons" escape hatch, valid precisely *because* the test is deliberately coupled to one backend.

See [concepts/backends.md](concepts/backends.md) for the backend-side rationale and the dx12 topic layout.

---

## Worked example: the transient system

The transient lifetime scope shows the split cleanly:

- **Tier 1** ([`tests/transient/transient-test.cc`](../tests/transient/transient-test.cc)) — the public
  contract, parametrized per backend: transient buffers have the requested shape, round-trip within their
  epoch, are mutually independent, expire once their epoch passes, and reuse (alias) storage across epochs;
  `set_budget` is deferred and repeatable, applied at the next `advance_epoch`; transient binding groups
  instantiate a layout and reject mismatched views.
- **Tier 2** ([`backends/dx12/tests/dx12-transient-test.cc`](../backends/dx12/tests/dx12-transient-test.cc)
  and [`dx12-compute-test.cc`](../backends/dx12/tests/dx12-compute-test.cc)) — dx12-specific internals: the
  64 KiB placement granularity of the bump heap, and end-to-end dispatch recycling over a deliberately tiny
  transient **descriptor ring** (which, unlike the buffer heap, is CPU-fed and so cannot bump-reset — see
  [`dx12_descriptor_heap`](../backends/dx12/src/shaped-graphics/backends/dx12/dx12_descriptor_heap.hh)).

---

## See also

- [nexus/docs/invocable-tests.md](../../../base/nexus/docs/invocable-tests.md) — the invocable/alias machinery.
- [concepts/backends.md](concepts/backends.md) — what a backend is and how it carries its own tests.
- [building-and-testing guide](../../../../docs/guides/building-and-testing.md) — driving `dev.py` + diagnostics.
