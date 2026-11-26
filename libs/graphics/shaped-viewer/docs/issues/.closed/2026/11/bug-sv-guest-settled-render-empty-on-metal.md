---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-viewer/tests/headless-viewer-test.cc:1200
  - test: "sv - a guest settled render advances no epoch of its own, and settles in its host's frames"
closed: 2026-11-26
resolution: fixed
---

# A guest settled render draws nothing on metal, and the run then hangs

On an Apple M4, relwithdebinfo, the test fails every time, run alone too (seed 1534530569865945033).
The render settles after about 4 s, but no pixel of the quad is covered: `covered` is 0 against 256.
The `settled` and epoch-count checks pass; only the image is empty.
The run then hangs with no tracked work outstanding, until the watchdog ends it at half sv's timeout, 300 s.
It came in with 3419bddba on 2026-11-12, which added the guest mode; on dx12 it is unknown.
Crash reports: `build/macos-arm-llvm-relwithdebinfo/crashes/shaped-viewer-test-20261113-111818-12180` (full suite) and `-112858-13690` (alone).

## Log

2026-11-13, measured with temporary probes, all reverted:
The test's host turns `pump_for(1.0)`, a millisecond, then advances, so between two guest frames it advances 10^4 to 10^6 epochs.
With the host sleeping 16 ms a turn the test passes, and still passes at 1000 advances a turn and at 0.5 s a turn.
At 20,000 advances a turn it fails as the spinning host does; so the trigger is thousands of epochs between guest frames, not wall time.
Each guest frame read back inside its own command list holds the right image (4864 nonzero bytes) until the settled frame, which writes all zeros, alpha too.
That frame records 2 draws and no dispatch, as it does in a passing run; its commit reports no feedback error, and the capture readback waits on it correctly.
Ruled out: the readback's ordering and copy-out, residency, the download ring's reservations, and bindless slots, since frames bind immutable snapshots.
Also ruled out: the view store's and resource pools' idle clocks; ticking them by guest frames instead changed nothing.
Not found yet: what in the display pass of a frame after a long epoch gap reads nothing; a GPU capture of that frame is the next step.
The metal staging rings keep one checkpoint per epoch and compact the vector from the front, so a host this fast also makes the run slow to end; that is the hang after the failure.

2026-11-13, later, the same way:
Raster mode fails too, so it is not the ray-tracing path: no BLAS, TLAS or wavefront state is needed.
Every routine is ready by the fourth frame in both runs; raster declines only while its depth and surface pipelines compile.
A frame that then executes (7 draws, 9 dispatches, 0.8 ms on the GPU, no feedback error) leaves the target all zero in some runs and right in others.
The accumulator of a wavefront run under the spinning host is all zero in every frame, while a slow host's holds the quad; samples still count up in both.
Also ruled out: the stream and off-frame transfer staging, which live until their own commit's feedback, and the overflow staging, which is made resident.
The working hypothesis is a resource a frame reads or writes being released or recycled by an epoch's retirement before the GPU runs that frame, since Metal 4 retains nothing; which one is not found.
A GPU capture in Xcode of a frame that executes and leaves nothing is the next step; it needs someone at the machine.

2026-11-13, a GPU capture tried:
Apple's capture layer hangs on `resolveCounterHeap`, sg's timestamp queries, so a capture needs them switched off; the bug does not need them.
With them off, every run that captured passed, even one capturing only the frames after the first 18 submissions: starting a capture drains the GPU, which hides the bug.
So an Xcode capture of the failing frame is not to be had either; the bug needs the GPU lagging behind the epochs.
Also ruled out by switching it off under the spinning host: aliasing in the transient bump heap, which resets its head every epoch; dedicated transients still fail.
And two guest frames in flight at once: one at a time still fails.

2026-11-13: the test's host is paced now, an advance every 4 ms as an app's frame loop would, and the test passes; this issue is the behavior under a host that is not.
To reproduce, put the host loop back to `pump_for(1.0)` then `advance_epoch()` with nothing between.
What a fix has to explain: a frame that runs on the GPU without error and leaves its target empty, once the host is thousands of epochs ahead of it.

2026-11-26, measured with temporary probes, all reverted:
Leaking every retired epoch's finalizers and allocators still failed, so nothing the epoch system frees was the cause.
Skipping the queue's epoch signal for epochs that committed nothing made the image whole, so the GPU's lag behind its signals was the trigger.
Never removing anything from the residency set made it whole too, at full spin, 17 million advances.
Bisected by site, the removal that matters is a binding group's: its argument buffer left the set the moment its last handle dropped, while its release waited for the epoch.
The queue reads the set when it hands a command buffer to the GPU, not at commit.
A frame queued behind millions of signals is handed over long after its groups were dropped, and its kernels read their arguments as zeroes.
A queue wait alone does not show it: a list held back by an unsignaled event keeps its residency, while one behind 100,000 epoch signals loses it in every run.

## Resolution

Every metal resource now leaves the residency set in the epoch-deferred step that releases it: buffers, textures, acceleration structures, binding groups and the software pool's storage.
`metal_residency_set`'s doc states the rule.
"sg metal - a binding group dropped while a submitted list still binds it stays resident until the list runs" pins it, failing 3 of 3 runs without the fix.
The hang after the failure was the staging rings' checkpoint vector compacted from the front once per retired epoch, quadratic in the epochs a fast host leaves; it is a `cc::ringbuffer` now.
The guest test keeps its paced host: unpaced it passes with the fix, but takes 5 to 97 s and up to 10 GiB retiring millions of epochs.
