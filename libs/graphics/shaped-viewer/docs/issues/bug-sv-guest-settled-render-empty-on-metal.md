---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-viewer/tests/headless-viewer-test.cc:1200
  - test: "sv - a guest settled render advances no epoch of its own, and settles in its host's frames"
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
