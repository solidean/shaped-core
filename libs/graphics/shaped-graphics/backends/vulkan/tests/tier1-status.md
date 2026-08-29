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

**A name containing a comma needs the comma escaped**, since a filter argument is comma-separated the way Catch2's is:

```bash
uv run dev.py test 'sg - a staging binding must be set\, even to nothing'
```

## Where it stands

**103 of 135 invocables pass.**

| topic | passing | of |
|---|---|---|
| `binding/staging-binding-group-test.cc` | 11 | 11 |
| `texture/texture-create-test.cc` | 11 | 11 |
| `error-handling/error-handling-test.cc` | 12 | 12 |
| `buffer/buffer-test.cc` | 10 | 10 |
| `transfer/transfer-test.cc` | 10 | 10 |
| `transient/transient-test.cc` | 10 | 10 |
| `binding/binding-group-array-test.cc` | 8 | 8 |
| `binding/bindless-array-test.cc` | 7 | 7 |
| `copy/copy-test.cc` | 6 | 6 |
| `context/context-test.cc` | 5 | 5 |
| `command_list/command_list-test.cc` | 4 | 4 |
| `raytracing/raytracing-test.cc` | 3 | 3 |
| `barrier/barrier-test.cc` | 2 | 2 |
| `query/query-test.cc` | 1 | 1 |
| `shader_package/shader_package-test.cc` | 1 | 1 |
| `transfer/stream-test.cc` | 1 | 16 |
| `transfer/transfer-fuzz-test.cc` | 1 | 2 |
| `transfer/download-async-test.cc` | 0 | 8 |
| `transfer/upload-async-test.cc` | 0 | 8 |

**Every remaining failure is one milestone.**
Async transfer and streaming — `upload-async`, `download-async`, `stream`, and `transfer-fuzz`'s async half — need the dedicated copy queue, which is the last thing to build.
Fifteen topics pass in full.

**A pass is not always coverage.** The three raytracing entries and the one query entry pass by `SKIP`ping, because `raytracing_is_supported()` and `query_timestamps_supported()` both answer false.
They turn into real coverage when those milestones land, and until then they prove only that the honest-false path works.

## The validation layer is the oracle

Both test tiers install a listener that fails the running test on any validation message of warning severity or worse,
the way the dx12 drivers do — `docs/testing.md` records that dx12 accumulated ~680 unnoticed messages before it grew one.

It is wired end to end rather than assumed: `sg vulkan - the debug messenger reaches the installed callback` provokes
a real VUID violation and checks the callback saw it.
That matters because a listener nobody has seen fire is indistinguishable from one that is not connected.

With it installed, the passing tests above pass **without producing a validation message** — which is a statement
about the backend rather than about the listener.

It earned its keep repeatedly during the bind path: a null descriptor needing `VK_EXT_robustness2`, a descriptor set
layout with two bindings at one index, and a pipeline cache outliving the device were all reported as named test
failures rather than found by reading code.

## What the remaining group is waiting on

- **Async and streaming transfer** — `upload-async`, `download-async`, `stream`, and one `transfer-fuzz` entry.
  These need the dedicated copy queue with its own timeline and copy actor, which is the last milestone.

Every failure here is a reported test failure rather than a teardown, from both stub kinds.
The `cc::error` creation stubs become typed exceptions through the throwing façade.
The `CC_UNREACHABLE` recording stubs are caught by nexus and reported as a failed check naming the seam — measured, not assumed: a full sweep with the driver enabled runs all 249 tests to completion.

**That holds where assertions are on, so in `relwithdebinfo-*` and not in `release-*`.**
With `CC_ASSERT` compiled out a `CC_UNREACHABLE` is undefined behaviour rather than a caught failure, which is the reason the driver stays `nx::config::disabled` rather than merely being left to fail.
