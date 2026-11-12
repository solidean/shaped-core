# Concept: backends

## What a backend is

`sg::context`, `sg::command_list` and `sg::raw_buffer` are **abstract interfaces**.
A *backend* is a self-contained static library under [`backends/`](../../backends/) that subclasses them directly and drives a real graphics API.
`sg::backend::dx12::dx12_context : public sg::context` is the shape.
dx12, vulkan and webgpu exist today, and a cpu reference, a capture layer or a remote context would be backends too.

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

## The dx12 runtime

**dx12 runs on the D3D12 Agility SDK's runtime where it was fetched, and on the one Windows carries otherwise.**
Shader Model 6.9, DXR 1.2 and shader execution reordering arrive only through the SDK, so a feature that needs them reports unsupported without it.
It is fetched on request, `uv run extern/agility/fetch-agility.py --accept-license`, since its terms are Microsoft's to accept rather than a build's; `SG_HAS_AGILITY` (1/0) says whether it was.

* **The runtime is one per process, and a process may ask for the system's.**
  `dx12::choose_d3d12_runtime` before the first device, else `SC_DX12_RUNTIME=system` or `agility`, else the SDK's where it was fetched.
  The default is the system runtime on an Intel GPU, until Intel's driver keeps its cache in an Agility process (`debt-sg-intel-keeps-off-agility`).
  The GPU it asks about is the one the context's search takes, `SC_GPU` included, so `SC_GPU=amd` on a machine with an Intel GPU too runs on the Agility SDK.
  A process that asked for the system runtime never creates the SDK's device factory, so on Intel's driver it keeps its shader cache.
  There the factory alone costs every later shader compile of the process its cache, vulkan's included.
  [The external entry](../../../../../docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels/readme.md) has the reproduction.
  `requested_d3d12_runtime()` says what was asked for, and `runs_agility_runtime()` what loaded.
* **No executable exports `D3D12SDKVersion`.**
  The backend loads D3D12Core.dll through `ID3D12SDKConfiguration1::CreateDeviceFactory`, so a static library chooses the runtime.
  It looks in a `D3D12` directory beside the executable first, which is how a shipped application would carry it, and in the fetched install otherwise.
* **Every device, debug-layer and DRED call goes through [`dx12_runtime.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_runtime.hh).**
  `D3D12CreateDevice` and `D3D12GetDebugInterface` directly would load the system runtime beside the SDK's.
* **One adapter has one device**, as the system runtime's singleton gives.
  The factory's own `ALLOW_RETURNING_EXISTING_DEVICE` hands out a second one regardless, and the backend's message routing assumes contexts on one adapter share theirs.
* **A capability is asked through `check_feature_support`**, which passes the SDK version the backend was built against, so nothing reported exceeds what that version knows.
* **A pipeline blob's key names the runtime**, since each runtime rejects the other's blobs.

## Backends carry their own tests

Each backend has its **own `*-test` binary** (`shaped-graphics-dx12-test`), built only where that backend builds.
The dx12 suite is gated to Windows by CMake, so it needs no `#ifdef`.
It runs on the hardware adapter, and on the **WARP** software adapter where there is none — a headless CI host — or under `--thorough`; [testing](../testing.md#devices-and-adapters) owns those rules.

Both adapters come from the same mechanism tier 1 uses: two **entry drivers** in [`dx12-entry.cc`](../../backends/dx12/tests/dx12-entry.cc).
Each brings up one context and awaits `nx::async_invoke_tests_in_sequence` over every invocable in the binary.
So the suite costs at most two devices rather than one per test — the shape it used to have, which was a throughput hazard and needed an `exclusive("gpu")` tag on every test to stay stable.
**That tag is gone.** Nothing in the dx12 suite is serialized against the GPU any more.
The two drivers and the tests that build a context of their own run concurrently at `-jN`, so several live `dx12_context`s are the normal state rather than an untested corner.
An alias per invocable keeps `dev.py test "sg dx12 - <name>"` selecting that one test, on both adapters.

A test whose subject is the context **itself** — pristine pool/epoch state, a backend knob, or two contexts — stays an ordinary `TEST`.
It takes one from `make_test_context()` in [`dx12-test-common.hh`](../../backends/dx12/tests/dx12-test-common.hh), on the hardware adapter or WARP where there is none.
Every context a test gets, either way, has the debug layer on and fails the test on an unexpected validation message; see [testing](../testing.md).

Two kinds of test belong in a backend suite.
**Feature smoke tests** — one end-to-end exercise per feature against a live device — and **backend-internal invariant tests** for behaviour invisible through the abstract surface.
Which test goes in which tier, and the rules for driving one, are [testing](../testing.md)'s job.

Tests are split **per topic**, one `.cc` per area, so the suite stays navigable as it grows:

```text
backends/dx12/tests/
  main.cc                     # nx::run entry point
  dx12-entry.cc               # the two entry drivers (warp / hardware) and the invocable aliases
  dx12-test-common.hh         # make_test_context() and the validation-message listener
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
