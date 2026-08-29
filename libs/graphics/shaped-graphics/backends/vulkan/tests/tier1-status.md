# vulkan against the tier-1 API suite

Which backend-agnostic sg API tests the vulkan backend passes today, and which it does not.
This file is **temporary scaffolding for the build-out** and is deleted in the commit that finishes the backend — at which point the answer is "all of them" and the file has nothing to say.

## How to reproduce it

The vulkan driver in [`vulkan-entry.cc`](../../../tests/backends/vulkan-entry.cc) is registered but `nx::config::disabled`.
Registering builds the per-invocable aliases; the disabled keeps a full sweep away from the recording seams that still abort.
So a tier-1 test runs against vulkan by naming it **exactly**, which enables it:

```bash
uv run dev.py test "sg - allocates a persistent buffer"
```

A substring pattern does not enable a disabled test, so `dev.py test "sg - "` runs the unconditional value-type tests and none of the invocables.
That is the whole reason this list is names rather than a pattern.

## Where it stands

**52 of 135 invocables pass.**

| topic | passing | of |
|---|---|---|
| `texture/texture-create-test.cc` | 11 | 11 |
| `error-handling/error-handling-test.cc` | 11 | 12 |
| `buffer/buffer-test.cc` | 9 | 10 |
| `context/context-test.cc` | 5 | 5 |
| `command_list/command_list-test.cc` | 4 | 4 |
| `raytracing/raytracing-test.cc` | 3 | 3 |
| `transfer/transfer-test.cc` | 3 | 10 |
| `transfer/stream-test.cc` | 2 | 16 |
| `binding/staging-binding-group-test.cc` | 1 | 11 |
| `transfer/upload-async-test.cc` | 1 | 8 |
| `transient/transient-test.cc` | 1 | 10 |
| `query/query-test.cc` | 1 | 1 |
| `barrier/barrier-test.cc` | 0 | 2 |
| `binding/binding-group-array-test.cc` | 0 | 8 |
| `binding/bindless-array-test.cc` | 0 | 7 |
| `copy/copy-test.cc` | 0 | 6 |
| `transfer/download-async-test.cc` | 0 | 8 |
| `transfer/transfer-fuzz-test.cc` | 0 | 2 |
| `shader_package/shader_package-test.cc` | 0 | 1 |

**A pass is not always coverage.** The three raytracing entries and the one query entry pass by `SKIP`ping, because `raytracing_is_supported()` and `query_timestamps_supported()` both answer false.
They turn into real coverage when those milestones land, and until then they prove only that the honest-false path works.

## The validation layer is the oracle

Both test tiers install a listener that fails the running test on any validation message of warning severity or worse,
the way the dx12 drivers do — `docs/testing.md` records that dx12 accumulated ~680 unnoticed messages before it grew one.

It is wired end to end rather than assumed: `sg vulkan - the debug messenger reaches the installed callback` provokes
a real VUID violation and checks the callback saw it.
That matters because a listener nobody has seen fire is indistinguishable from one that is not connected.

With it installed, the 52 passing tests above pass **without producing a validation message** — which is a statement
about the backend rather than about the listener.

## What each remaining group is waiting on

Every failure so far is a missing milestone rather than a defect — checked one by one, including the four topics that fail exactly one test.

- **Inline transfer and barriers** — `transfer`, `copy`, `barrier`, `transfer-fuzz`, and the transfer-shaped tests inside other topics.
  Blocked on the host-visible staging path: nothing in the backend requests a `HOST_VISIBLE` memory type, so there is no ring to copy through.
- **The bind path** — `binding-group-array`, `bindless-array`, `staging-binding-group`, and `error-handling`'s one failure.
  `try_create_binding_group_layout` and its four siblings are still `cc::error` stubs.
- **The transient scope** — `transient`, and `buffer`'s one failure (`create_buffer<T> (transient)`).
  `try_create_memory_heap` is a `cc::error` stub, and `ctx.transient` bump-allocates out of one.
- **Async and streaming transfer** — `upload-async`, `download-async`, `stream`.
  These need the dedicated copy queue, which is the last milestone.
- **`shader_package`** — needs a SPIR-V producer; there is no shader compiler on Linux yet.

Every failure here is a reported test failure rather than a teardown, from both stub kinds.
The `cc::error` creation stubs become typed exceptions through the throwing façade.
The `CC_UNREACHABLE` recording stubs are caught by nexus and reported as a failed check naming the seam — measured, not assumed: a full sweep with the driver enabled runs all 249 tests to completion.

**That holds where assertions are on, so in `relwithdebinfo-*` and not in `release-*`.**
With `CC_ASSERT` compiled out a `CC_UNREACHABLE` is undefined behaviour rather than a caught failure, which is the reason the driver stays `nx::config::disabled` rather than merely being left to fail.
