---
created: 2026-11-06
---

# A device loss on the Arc under a repeated sg suite, which an assert turns into an abort

`uv run dev.py test shaped-graphics-test --repeat 20` lost the vulkan device once, on iteration 8, on 2026-11-06 on the Intel Arc B580.
A rerun of 12 iterations passed, and no Windows driver-timeout event (4101) was logged, so nothing shows what lost it.
The test running at the time was `sg - a strong compare-exchange in a pixel stage leaves helper pixels out, and the draw ends`, which is only where the loss was noticed.

## Seen again on 2026-11-07

`--repeat 8` crashed on iteration 7 on the Arc B580, with `print` and `assert` lowered in legalize (43b1753d4) and geometry and tessellation merged.

- **It is the GPU rather than vulkan.** vulkan logged `VK_ERROR_DEVICE_LOST`, and within 0.2 s dx12's debug layer logged `DXGI_ERROR_DEVICE_HUNG` in the same process.
- **`compute-float-specials-test.cc` read zeros on every line**, which is a dispatch that never ran rather than a float bug; it is one more place the loss was noticed.
- **The abort was the assert in `vulkan_upload_async.cc`** at that time, `vkQueueSubmit (async upload) failed`; see below.

**The legalize change did not raise the rate, as far as a comparison can show.**
Across those earlier runs, 3 of about 34 iterations crashed against 1 of 32, which was too few to read.
So `--repeat` alternated 3b9189646, before native `print`, with f7232f75a, which also holds mesh and task, four rounds of each.

| side | iterations counted | device-loss aborts |
|---|---|---|
| before | 40 | 2 |
| after | 40 | 2 |

Each side also had two rounds cut off by a 175 s cap and not counted.
Those rounds logged no crash, and no 20-iteration run fitted the cap, so the cap rather than a hang is the likely reason.
All four aborts are `vkQueueSubmit` failing or `VK_ERROR_DEVICE_LOST`, at different tests each time, so the test running is where the loss is noticed rather than what causes it.
This is a rate of very roughly one iteration in twenty to forty, so one `dev.py ship` meets it now and then.
No hung submission has been identified, so what hangs the GPU is unknown.

## What a loss does in an async transfer now

An async upload or download whose `vkQueueSubmit` loses the device marks it lost and settles the job as cancelled, where it used to assert.
A download must not go on, since its staging window holds none of the job's bytes and would be delivered as a success.
Forcing `VK_ERROR_DEVICE_LOST` on the fifth submit of each system shows the loss reaching tests as `sg::device_lost_exception`, named `vkQueueSubmit (async upload)` or `(async download)`.
Nothing keeps that under test: no seam can fail a submit, so the check was a temporary mutation.

## Measured on 2026-11-07, afternoon

**Every abort after a loss had a cause of its own, and each is fixed.**
A real loss now fails the run's tests instead of terminating the binary, and a crash report comes out whole.
A forced one, a `vkQueueSubmit` made to fail with the GPU still running, no longer aborts but hangs a later wait instead.
That is the mutation's doing rather than a real loss's, whose waits return `VK_ERROR_DEVICE_LOST`, so the forced loss stops being a test of this path.

- **The empty reports were the crash reporter.** The CRT resets `SIGABRT` before calling the handler.
  So a second thread's abort ended the process mid-report as 0xC0000409, and after a loss many threads fail at once.
  It now waits for the first report.
- **dx12's submit** threw over a list it had executed, whose destructor then asserted on `Close`.
- **`~context`** asserted that no resource outlived it, which the work a loss fails always breaks; on a lost device it logs that instead.
- **Five tests** advanced the epoch in a `CC_DEFER`, which throws a second time in the unwind of the first loss.

**The doubled timeline signal is a symptom.**
VUID-03242 shows up only after a loss: the validation layer still counts a failed submit's signals as pending, and the cancel path signals the value again.
That second signal stays, because without it a wait on the value never returns where a submit failed but the GPU did not hang.
A forced loss showed it: dropping the signal turned the run into a hang.

**The loss needs neither the other API nor the Agility SDK.**
Without Agility, on the Arc B580:

| configuration | clean iterations | losses |
|---|---|---|
| the whole suite | 40 | 1, on its second iteration |
| the `vulkan` context alone | 25 | 1 |
| the `dx12-hw` context alone | 100 | none |

**So it is vulkan's to explain.**
dx12 alone clears the bar of 100 iterations, and vulkan alone keeps the loss without dx12 beside it.
dx12 reports `DXGI_ERROR_DEVICE_HUNG` in the whole suite only because the two share one hung GPU.
It lands in a different test each time, Windows logs no driver timeout (4101) or TDR for it, and the test it is noticed in passes 40 of 40 repeated on its own.
What on vulkan hangs the GPU is still unknown.

**Narrowed further, later on 2026-11-07, without Agility.**

| configuration | clean iterations | losses |
|---|---|---|
| the `vulkan` context alone, validation layers and sync validation off | 59 | 1 |
| tests named `sg stream - `, on every context | 100 | none |
| tests whose name holds ` SGL `, on every context | 120 | none |

- **The validation layers are not needed.** With them off, the loss came on iteration 60, so they neither cause it nor hide a message that would name it.
  Its first symptom was a procedural-sphere trace reading zeros, another test the loss is only noticed in.
- **The groups are too small to clear anything.** The `vulkan` context runs some 440 tests an iteration and loses the device about once in 26.
  A loss spread evenly over them would show in a group holding a tenth of them about once in 300 iterations, so 100 clean ones exclude little.
  A dev.py test pattern is a substring or a file glob, and a file glob reaches none of the invocable tests a context runs.
  So halving the context needs a split by name that a pattern cannot express, and was not done.

**The stale readback does not race on its own.**
That test, repeated alone on every context, ran 240 iterations clean.
Reading the actor: the submit stamps the latest pending upload value while the list still holds the buffer, and drops the handle after.
The actor reads the handle count before the stamp, and only when the count is zero, so on x86 the compiler cannot move that read after the stamp's.
So it needs other work beside it, and is more likely one more symptom of the GPU going wrong than a race of its own; it is not excluded.

**Not done:** why a real loss exits 0xC0000409 rather than the abort handler's 3 now that the report completes, which is `crash_handler.cc`'s and left to it.

**Bisected with `--shard`, the same day.**
nexus's `--shard K/N` splits the `vulkan` context's children by a hash of their names, so the halves are disjoint and stable.

| shard of the `vulkan` context | clean iterations | failures |
|---|---|---|
| 0 of 2 | 100 | none |
| 1 of 2 | 34 | 1: a `mat3` test read nine of 64 matrices back as another resource's bytes, with no loss |
| 1 of 4 | 100 | none |
| 3 of 4 | 100 | none |

So no quarter does it on its own, though the half that holds both quarters failed.
Each quarter's iteration is about two seconds, against seven for the whole context, so either the cause takes tests from both quarters or it takes a longer run's worth of work.
That points at state the run builds up rather than one test.
The resource use over a run was not measured.

The `mat3` failure is the same kind as the stale readback: foreign bytes where a buffer's own should be.
The other four buffers of that test read back right, and the bytes it got were plausibly another test's, which suggests one allocation's memory in use by a second live resource.
That is a hypothesis; it may be what a loss looks like when it does not hang the GPU.

**Out-of-bounds shader access is excluded.**
sg enables only `nullDescriptor` of `VK_EXT_robustness2`, so on vulkan an out-of-bounds access is undefined where dx12's descriptors bound it, which fitted dx12 alone running clean.
With `robustBufferAccess`, `robustBufferAccess2` and `robustImageAccess2` turned on, the `vulkan` context still lost the device, on iteration 36.
That covers descriptor-bound buffers and images; an out-of-bounds buffer device address is not bounds-checked by either, so that is not excluded.
GPU-assisted validation stalled the context in an async-upload test before reporting anything, and was given up.

**Async transfer is in every loss, and could not be taken out.**
The tests a loss is first seen in are async upload, async download and stream tests more often than not, and the first abort of all was `vkQueueSubmit (async upload)`.
Forcing transfers onto the graphics queue, the fallback for a device with no spare queue, hangs the first iteration instead.
That is [bug-sg-vulkan-transfer-fallback-deadlocks](bug-sg-vulkan-transfer-fallback-deadlocks.md).
So whether the loss needs the transfer queues is still open, and making that fallback work is what would answer it.

**On the Agility SDK, the loss on every run is another bug.**
Intel's dx12 driver removes the device creating one hit-object pipeline, through DXC's DXIL and SGL's alike.
[intel-d3d12-hit-object-pipeline-removes-device](../../../../../docs/bugs-external/intel-d3d12-hit-object-pipeline-removes-device/readme.md) is the write-up.
With that one test skipped, 25 whole-suite iterations on Agility lost nothing, against 9 losses in 9 before.

**A readback can be wrong without a loss.**
`sg - an async upload a submitted list reads is copied even after the last handle is dropped` read stale bytes once on `vulkan-native-spirv`, in the 42 whole-suite iterations above.
That test pins a race between the upload actor skipping a copy whose target nobody holds and a list waiting on its value.
Whether the actor can read the handle count and the awaited stamp out of order is unproven, and it may be the readback that precedes a loss.

## Seen again on 2026-11-08

A single `uv run dev.py test shaped-graphics-test`, no `--repeat`, lost the vulkan device 2.9 s in, while a fuzz run's first `mk_trace` waited (seed 1530684561).
Every vulkan test after it failed, and dx12's readbacks read zeros within the same second, as in the sightings above.
The next run of the same binary passed whole.
It was the first GPU run of a change that rewrote most native shaders, so the driver compiled them all anew.

Again later that day, in `dev.py ship`'s test step: the stream tests' readbacks failed 6 s in, then `vkCreateSemaphore` asserted and the process faulted.
It was again the first GPU run after native shaders changed, `hoist_common_chains` on every SGL module, with the drivers' caches cold.
Two whole runs of the same binary straight after passed all 1980 tests.

## Seen again on 2026-11-09

In `dev.py ship`'s test step, on the first GPU run after SGL's ray-tracing lowering changed: `trace_ray`'s payload is read back from its last argument now.
So every native ray-tracing shader compiled anew.
`software-trace-test.cc`'s motion readbacks read zeros 4 s in, a fuzz run's wait reported `VK_ERROR_DEVICE_LOST`, and every vulkan test after it failed.
The `sg vulkan backend` tests then passed twice in a row on their own, and the next `ship` passed whole.
