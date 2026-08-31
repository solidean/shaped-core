# Testing shaped-graphics

sg is strongly test-driven, and its tests split into **two tiers** by what they pin down.
Getting a new test into the right tier is what keeps the public contract validated *once* across every backend, with backend-specific machinery tested where it lives.

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
INVOCABLE_TEST("sg - transient buffer round-trips within its epoch", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    // ... drive ctx-> through the public API only ...
}
```

It becomes runnable against each backend by two pieces working together:

- **Entry drivers** — [`tests/backends/<backend>-entry.cc`](../tests/backends/) create a concrete context (dx12 on WARP, …) and `nx::invoke_tests("<backend>", ctx)` every invocable against it.
  A backend that cannot come up `SKIP`s.
  A backend still being built out **registers but disables its driver**, which is how vulkan was grown.
  Registering defines the aliases, so any one API test runs against it by being named exactly.
  The `nx::config::disabled` keeps a sweep out of the seams it has not reached — where a stub aborts, a sweep is a crash rather than a set of failures.
  Nexus's orphan check exempts an alias-reachable invocable for exactly this case, so the suite stays green while the backend grows.
  The disabled comes off once no seam aborts, and both backends now sweep.
- **Alias setup** — [`tests/backends/backends.cc`](../tests/backends/backends.cc) defines, per invocable, an alias of the same name expanding to one scoped run per registered backend.
  So `dev.py test "sg - <name>"` runs it on whichever backends this binary was built with.

Full mechanism: [nexus/docs/invocable-tests.md](../../../base/nexus/docs/invocable-tests.md).

### A validation message fails the test that provoked it

The dx12 drivers create their context with the debug layer on and install a listener via `dx12_context::set_message_callback`, failing the running test on any message of `warning` severity or worse.
Without it a validation error is a line on stderr nobody reads, and the run stays green — which it did, for ~680 of them.
Attribution rides the ambient context, so the check lands on the right test wherever the runtime raised the message.

A test whose subject **is** the bad input opts out with a `dx12::scoped_expected_validation_messages` guard; there is no tag for it.
That guard is thread-scoped rather than per-context, and deliberately so: D3D12 hands one message to **every** callback registered in the process, not only the one on the device that raised it.
With several contexts alive — the normal state of the dx12 suite at `-jN` — silencing one context's listener leaves the others to fail the test anyway.
The message is raised synchronously on the thread that provoked it, so the thread is what names the right test.

sg's own warnings go through `CC_LOG_WARNING`, so they are **events rather than terminal output** and a test can assert on one.
Register a `cc::rec::recording_listener`, provoke the warning, and ask the recording whether it fired — `tests/barrier/slot-recording-test.cc` is the worked example.
That is also how you pin a warning firing exactly ONCE, which the hand-rolled `warned` guards around them exist to guarantee.

**What belongs here:** every statement about the public API — allocation shapes, lifetime/epoch semantics, transfer round-trips, binding validation, the transient budget contract.
Anything that must hold for dx12 *and* vulkan *and* a future cpu backend goes here, written once rather than duplicated per backend.
Complex and edge-case coverage belongs here too.
Drop to tier 2 only when you genuinely need backend internals or a backend-specific resource, such as an embedded shader blob.

Tests are split **per topic**, one `.cc` per area (`buffer/`, `transfer/`, `binding/`, `transient/`, …),
and each topic file is added to the `if(_sg_test_drivers)` block in the library
[`CMakeLists.txt`](../CMakeLists.txt) (agnostic tests need at least one backend to run against).

> A backend-agnostic test still needs *some* backend to execute.
> Until an always-available CPU/validation backend exists (a TODO in `CMakeLists.txt`), tier 1 runs only where a real backend builds — dx12 on Windows, vulkan wherever the SDK is.

---

## Tier 2 — per-backend suites (`backends/<backend>/tests/`)

Each backend has its **own `*-test` binary**, built only where that backend builds, and running on a software adapter where possible (dx12 → WARP) so it also runs on headless CI.
Two kinds of test belong here:

1. **Feature smoke tests** — one straightforward end-to-end exercise per feature, confirming the backend's own path works against a live device.
   Not the full public semantics; tier 1 does that.
2. **Backend-internal invariants** — behaviour invisible through the abstract surface.
   Descriptor-ring and ring-buffer reclaim, bump-allocator placement granularity, command-list/allocator pooling, epoch-gated recycling.
   These `static_cast` the handle to the concrete context and inspect its guts.
   That is the legitimate "here be dragons" escape hatch, valid precisely *because* the test is deliberately coupled to one backend.

### A tier-2 test is an invocable too, unless it needs its own context

The dx12 suite has the same driver shape as tier 1.
[`dx12-entry.cc`](../backends/dx12/tests/dx12-entry.cc) brings up one WARP and one hardware context, and invokes every `INVOCABLE_TEST` in the binary against each.
So the default for a new tier-2 test is `INVOCABLE_TEST("sg dx12 - …", (dx12::dx12_context_handle const& ctx))`, which also gets it exercised on the real GPU for free.
The parameter is the **backend-typed** handle, unlike tier 1's `sg::context_handle`: a suite committed to one backend should not have to downcast to read its guts.

Write an ordinary `TEST` only when the test needs a context of its own: pristine epoch / pool state, or a `dx12_config` knob it is about.
`dx12::make_test_context({…})` in [`dx12-test-common.hh`](../backends/dx12/tests/dx12-test-common.hh) is how to get one, and such a test carries `exclusive("gpu")`.

### Drive through the abstract API even though the handle is backend-typed

A tier-2 test holds a `dx12_context_handle`, so every backend method is one `->` away.
That is a convenience for **reading guts**, not a licence to drive with them.
The work still goes through the abstract surface: `ctx->uncached` / `ctx->persistent` / `ctx->cached` for resources and schemas.
Recording likewise — `ctx->create_command_list` / `ctx->submit_command_list` / `ctx->wait_for`.

The anti-pattern is using the backend-typed methods as the main driver:

```cpp
auto buf = ctx->create_dx12_buffer(size, usage, {});   // WRONG as a driver: bypasses the public contract
ctx->submit_dx12_command_list(...);
```
Written this way the test exercises the backend's private API instead of the contract every backend must honour, and silently stops being portable.
Prefer the public form — `ctx->persistent.create_raw_buffer(...)`, `ctx->submit_command_list(...)`.
Reach for the backend only in an assertion, where the abstract surface has nothing to say: `CHECK(ctx->_cmd_pool.free_allocator_count(…) == …)`, `CHECK(ctx->_descriptor_heap.watermark == …)`.
The same rule holds for integration tests in dependent libraries such as `shaped-shader-compiler-dxc/tests`: create the concrete context as the entry point, then drive it as a plain `sg::context&`.

**A tier-2 test may name backend API in exactly three places — everything else routes through the `sg::context` surface:**

1. **Entry point** — `dx12::make_test_context({...})` (or `sg::create_<backend>_context`) to bring a context
   up, including to set a backend-specific knob the test is about (ring sizes, descriptor-heap capacity, …).
2. **Inspection** — a backend member or a resource downcast *inside an assertion*, to read internal state
   the abstract surface doesn't expose.
3. **Backend-exclusive resources** — features with no public `sg` entry point yet (e.g. dx12 RTV/DSV
   descriptors). Legitimately backend-typed end to end; keep the backend-typed span minimal and say why.

Self-check when writing or reviewing a tier-2 test: every `create_<backend>_*` / `submit_<backend>_*` outside an assertion is the smell.
Search the file for them and confirm each surviving one is case 1, 2 or 3 above.
A call that is none of those has a public form — use it.

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
