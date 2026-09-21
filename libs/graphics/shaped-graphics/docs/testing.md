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

## Devices and adapters

These rules bind every GPU test in the repo — sg's two tiers, and the libraries above sg (sr, sv, the shader compiler) alike.

- **A test binary shares its devices.** One context per adapter, brought up by an entry driver that invokes every test against it, which is the tier-1 shape below.
  A device is expensive to create and to tear down, and several alive at once contend in the driver.
  A test builds a context of its own only when the context itself is its subject — creation, teardown, a config knob, pristine epoch or pool state.
- **The hardware adapter is the default.** It is what the code ships on, and it is fast.
  WARP runs where there is no hardware adapter, which is what a headless CI host is, and under `--thorough` as a second pass on a machine that has one.
  The WARP drivers ask `dx12::has_hardware_adapter()` and `nx::is_thorough()` and skip otherwise.
  A hardware driver skips only when there is no hardware adapter; one that exists and still fails to create a device fails the test.
  `SC_DX12_ADAPTER=warp` hides every hardware adapter from a whole process, which is how to reproduce a GPU-less run locally: the hardware drivers skip and the WARP ones run.
  `SC_SG_COLD=all` adds the other half of a CI runner, a first build of every shader and pipeline: the persistent tiers are neither read nor written.
  `pipelines` or `shaders` turns off one of the two, and the driver's own cache is out of its reach.
- **A test passes on any adapter.**
  Hardware and WARP differ in precision, in timing and in what a driver does with a blob, and a test is written against the contract rather than against one of them.
  Pinning a test to an adapter is reserved for a **known bug** in that adapter, named where it is pinned, and the list of those stays short.
- **Never assert bytes that depend on the adapter.**
  A serialized PSO, a floating-point readback compared exactly, or anything else one driver produces differently from another is not a stable expectation.
  Compare within a tolerance, or assert the property instead.

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

- **Entry drivers** — [`tests/backends/<backend>-entry.cc`](../tests/backends/) create a concrete context and `co_await nx::async_invoke_tests_in_sequence("<backend>", ctx)` over every invocable.
  The dx12 ones follow [Devices and adapters](#devices-and-adapters): the hardware adapter by default, WARP where there is none or under `--thorough`.
  A backend that cannot come up `SKIP`s.
  A driver holds no exclusion tags: the async invocation takes each child's own (`sg-reload-generation`) around its run.
  So a test that counts routine init runs carries the tag itself, and a driver that also held it would be refused.
  **Shaders are not one of those tests.**
  This binary has exactly one `slib::shader_library`, in [`tests/shaders/shader_fixtures.cc`](../tests/shaders/shader_fixtures.cc), holding every compiler the build has.
  **A driver brings it up before it invokes**, so an invocable simply acquires through the generated package globals and says nothing about where the library came from.
  Nothing needs excluding, and a shader compiles once for the whole run rather than once per test.
  **A test awaits the GPU rather than blocking on it.**
  A readback awaits its own result: `auto const data = co_await future.data();`.
  `co_await ctx->idle_completion()` is for a test that needs the whole GPU and every actor drained.
  `co_await ctx->routines.idle_completion()` is for one that needs every registered routine up.
  A blocking wait in a library's tests is a `blocking-wait` lint finding, allowed by file in that library's `.shaped-lint.yml` where the wait is the point.
  A backend still being built out **registers but disables its driver**, which is how vulkan was grown.
  Registering defines the aliases, so any one API test runs against it by being named exactly.
  The `nx::config::disabled` keeps a sweep out of the seams it has not reached — where a stub aborts, a sweep is a crash rather than a set of failures.
  Nexus's orphan check exempts an alias-reachable invocable for exactly this case, so the suite stays green while the backend grows.
  The disabled comes off once no seam aborts, and every backend now sweeps.
  **The whole sweep runs a second time under a browser's rules**, through a `never_block` driver per native backend: any sg call that would wait on the caller's thread asserts there.
  The dx12 one runs on WARP in every default run, and the vulkan one only under `--thorough`.
  So on Linux the default run exercises `never_block` only on wasm, through the webgpu driver, which is never-block by nature.
- **Alias setup** — [`tests/backends/backends.cc`](../tests/backends/backends.cc) defines, per invocable, an alias of the same name expanding to one scoped run per registered backend.
  So `dev.py test "sg - <name>"` runs it on whichever backends this binary was built with.

Full mechanism: [nexus/docs/invocable-tests.md](../../../base/nexus/docs/invocable-tests.md).

### A validation message fails the test that provoked it

Every driver creates its context with validation on and installs **no listener**: the backend logs each message at the layer's own severity.
nexus's log rule then fails the test that logged a warning or worse — [log-rule.md](../../../base/nexus/docs/log-rule.md).
Without that a validation error is a line on stderr nobody reads, and the run stays green — which it did, for ~680 of them.
Attribution rides the ambient context, so the failure lands on the right test wherever the runtime raised the message.

The advisories sg provokes on purpose are [`dx12_expected_messages.hh`](../backends/dx12/src/shaped-graphics/backends/dx12/dx12_expected_messages.hh).
Each dx12 driver allows them for every test in its binary.
A test whose subject **is** the bad input declares it: `nx::expect_error(...)` when the message is the point, `nx::allow_errors(...)` when whether it appears depends on something else.

D3D12 hands one message to **every** callback registered on the device that raised it, and two contexts on one adapter share a device.
So the backend logs a message from one context per device only — the oldest there without a listener — and a test sees it once however many contexts are alive.
The broadcast stops at the device: a WARP context never sees a hardware context's messages, so deduplicating across the process would silently drop one adapter's.
`dx12-validation-broadcast-test.cc` pins both halves.

A loader notice about software installed on the machine, such as a screen recorder's implicit layer announcing an older API version, is logged at `info`.
Nothing in sg can act on it, and a test must not fail on what is installed.

sg's own warnings go through `CC_LOG_WARNING`, so they are **events rather than terminal output**, and a test that provokes one declares it with `nx::expect_warning`.
That is also how you pin a warning firing exactly ONCE — `nx::exactly(1)` — which the hand-rolled `warned` guards around them exist to guarantee.

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

Each backend has its **own `*-test` binary**, built only where that backend builds, and runs on the adapters [Devices and adapters](#devices-and-adapters) prescribes.
Two kinds of test belong here:

1. **Feature smoke tests** — one straightforward end-to-end exercise per feature, confirming the backend's own path works against a live device.
   Not the full public semantics; tier 1 does that.
2. **Backend-internal invariants** — behaviour invisible through the abstract surface.
   Descriptor-ring and ring-buffer reclaim, bump-allocator placement granularity, command-list/allocator pooling, epoch-gated recycling.
   These `static_cast` the handle to the concrete context and inspect its guts.
   That is the legitimate "here be dragons" escape hatch, valid precisely *because* the test is deliberately coupled to one backend.

### A tier-2 test is an invocable too, unless it needs its own context

Both backend suites have the same driver shape as tier 1.
[`dx12-entry.cc`](../backends/dx12/tests/dx12-entry.cc) brings up one hardware and one WARP context, and invokes every `INVOCABLE_TEST` in the binary against each.
The WARP one runs only where the adapter rules call for it.
[`vulkan-entry.cc`](../backends/vulkan/tests/vulkan-entry.cc) brings up one context, with validation and synchronization validation on, and SKIPs where there is no Vulkan device.
So the default for a new tier-2 test is `INVOCABLE_TEST("sg dx12 - …", (dx12::dx12_context_handle const& ctx))`, or `vulkan::vulkan_context_handle` for vulkan.
The parameter is the **backend-typed** handle, unlike tier 1's `sg::context_handle`: a suite committed to one backend should not have to downcast to read its guts.

The invocables under one driver run in turn on one context, so each leaves it as it found it.
Every list is submitted or dropped, and a test that swaps the validation callback reinstalls the failing one before it returns.
An assertion on a counter the context has already advanced — the epoch, a pool's free count — is written against a snapshot taken at the start of the test rather than against zero.

Write an ordinary `TEST` only when the context itself is the subject: pristine epoch / pool state a snapshot cannot stand in for, a backend config knob, or more than one context.
`dx12::make_test_context({…})` in [`dx12-test-common.hh`](../backends/dx12/tests/dx12-test-common.hh) is how to get one, and `make_fresh_context()` is the same with no knobs.
Both take `dx12_adapter::hardware_or_warp`, so such a test follows the adapter rules too.
The vulkan one is `vulkan::test::make_context({…})` in [`vulkan-test-common.hh`](../backends/vulkan/tests/vulkan-test-common.hh), and its test carries `exclusive("vulkan-device")`.

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
