# Vulkan validation layer: false WRITE_RACING_READ after a wait on a not-yet-signalled timeline value

**Status:** fixed upstream in Vulkan SDK 1.4.350; `dev.py` warns about anything older.
**Affects:** `VK_LAYER_KHRONOS_validation` with synchronization validation, SDK 1.4.304 through 1.4.341.
We hit it on 1.4.313 and confirmed the fix on 1.4.357.
**Found by:** `shaped-graphics-test` failing at random with validation errors attributed to no test.

## What happens

A submit can wait on a timeline semaphore value that no queue has signalled yet.
The layer defers that batch until the signal arrives, which is correct.
But when some *other* timeline signal is recorded in the meantime, the layer processed its deferred batches and dropped them, including ones whose waits were still unresolved.
A dropped batch is never validated and its own signals are never recorded, so later waits on it resolve late and out of order.

Synchronization validation then believes two submissions on different queues overlap when the application serialized them, and reports a hazard that did not happen.

## How it showed up in shaped-core

The vulkan entry driver runs the whole tier-1 suite with synchronization validation on, and its listener fails a test on any message.
It failed roughly once in ten runs of `shaped-graphics-test`, with this, repeated about ten times:

```raw
vulkan validation: vkQueueSubmit(): WRITE_RACING_READ hazard detected. vkCmdCopyBuffer (from VkCommandBuffer 0x20119d21d60
submitted on the current VkQueue 0x200d8f08f60) writes to VkBuffer 0x1510000000151, which was previously read by another
vkCmdCopyBuffer command (from VkCommandBuffer 0x20119d83000 submitted on VkQueue 0x200d8be96e0).
No sufficient synchronization is present to ensure that a write (VK_ACCESS_2_TRANSFER_WRITE_BIT) at
VK_PIPELINE_STAGE_2_COPY_BIT does not conflict with a prior read (VK_ACCESS_2_TRANSFER_READ_BIT) at the same stage.
```

Our transfer design is exactly the pattern that trips it.
A command list touching a buffer with a queued async transfer waits on a timeline value reserved when the transfer was enqueued, before the transfer thread has submitted the copy that signals it.
That is a wait-before-signal on purpose, and it is valid Vulkan.

Three things made it look like our bug and were not:
- The message named a test that never touches a transfer queue.
  The async upload and download threads that submit have no test attached, and a deferred batch is validated inside whichever later submit resolves it.
- The three recurring writer command buffers matched the async upload's three staging windows, so the writes really were ours.
- More overlapping tests made it more frequent, which reads like a race in our code rather than in the layer's bookkeeping.

## Why it is the layer and not us

Same binary, same machine, only the layer swapped through `VK_LAYER_PATH`:

| layer | `shaped-graphics-test --repeat` | result |
|---|---|---|
| SDK 1.4.313 | 30 | failed on iteration 9, 20 WRITE_RACING_READ lines |
| SDK 1.4.357 | 100 | all passed |

## Where the diagnosis comes from

We did not find this in an issue tracker.
It came from reading the layer's submit-time validation source (`layers/sync/sync_validation.cpp`, `layers/sync/sync_submit.cpp`) for 1.4.313 and a newer release side by side.
The upstream history then names it:

- **Introduced by** [PR #8702](https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/8702), "sync: Refactor code that updates unresolved batches".
  Merged October 2024, first shipped in SDK 1.4.304.
- **Fixed by** [PR #12156](https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/12156), "sync: Fix regression due to old refactor", first shipped in SDK 1.4.350.
  The commit is [bfdde0c](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/bfdde0c61dfd05bdfa979cf73bf6f61e19f515a8).
  Its new positive test, `PositiveSyncValTimelineSemaphore.WaitBeforeSinglaBlocksAnotherSubmit`, is "a batch blocked by a wait-before-signal batch is not validated" — our pattern, minimized.

That upstream test is the standalone reproduction, which is why this entry has no `repro.cc` of its own.

## What we do about it

Nothing in the library.
A `VULKAN_SDK` older than 1.4.350 is a known issue in `tools/dev/lib/toolchain/known_issues.py`.
`dev.py doctor` shows it as a warning, and every command that runs our binaries prints it before it starts.
`vulkan-entry.cc` says why next to where it enables synchronization validation.

## Retiring it

Once no supported machine or CI image carries an SDK older than 1.4.350, drop the doctor check and this directory.
