---
created: 2026-11-02
tags: [flaky, ci]
refs:
  - file: .github/workflows/ci-windows-clang.yml
  - test: "sv dx12 - warp"
---

# shaped-viewer-test hangs on the Windows Clang CI's WARP run, in a different test each time

Every Windows Clang run on main on 2026-11-02 failed in shaped-viewer-test: nexus' watchdog killed it after no test started or finished for 300 s (exit code 4), once an access violation instead.
The test awaited under "sv dx12 - warp" differs per run:
- run 41482062516: "sv - a capture that times out writes beside the requested path, not to it";
- run 41504898877: "sv - a view accumulates across frames under its id";
- run 41522700574: "sv - a capture writes a complete image and ends the loop".

In every report the main thread waits in `participate_until_ready` under `nx::execute_tests`, the pool's workers are idle, and the watchdog lists no tracked work in flight.
So something the test awaits is never resolved: hypothesis, a frame loop or capture waiting on a GPU fence WARP never signals, or an untracked wake that is lost; not shown.
- Locally (Windows, relwithdebinfo-clang, hardware adapter) the binary passes in 76 s; WARP runs there only under `--thorough`, so the default local run never takes this path.
- The same runs saw clean-core-test exit 4 twice (41494273936, 41516332305), also a watchdog kill; whether it shares the cause is not known.
- The diagnostics artifact keeps the crash report and minidump under `crashes/incoming/`.

## Reproduced locally (2026-11-03)

`uv run dev.py test "sv dx12 - warp" --thorough` on relwithdebinfo-clang hangs the same way within 5 to 10 minutes, twice in two runs.
CI's run 41622360612 hung in "sv::hybrid_routine - a glass pane thinner than a millimeter shows the sky through it".
The local runs hung in "sv - a decal coming, changing or going restarts the view's accumulation" and "sv - view renderer renders indexed geometry (headless)".

- **Where it waits (measured):** in the last, view-renderer-test.cc:132, `sv_test::frames_until_executed`, in `cc::async_blocking_get(ctx.idle_completion())`.
  That blocking get runs on a pool worker: the async test's coroutine is polled by `async_thread_pool::worker_main` and blocks inside it, in `participate_until_ready`.
  So `idle_completion()` never resolves after a frame's submit and advance.
- **Ruled out (measured):** `sv::background_work`'s settle at the end of each test; a probe logged both its calls in the hung run as entered and settled.
- **Not visible:** the dx12 completion waiter thread, whose park would show `WaitForMultipleObjects`; the hang report stops after 64 threads and WARP's own workers fill them.
  Raising that cap, or naming the waiter thread and listing named threads first, would show whether it is parked, on which targets, or gone.
- **Hypotheses, unverified:** a completion that needs a pool thread to run (a readback actor's copy) while workers block in synchronous waits inside async tests.
  Or a lost wake in the waiter's arm/park, which a fence WARP signals with different timing would expose.

## The hang is inside WARP (2026-11-03)

A trace of every completion, arm, park and wake showed both hypotheses above wrong: the waiter is parked on the right submission's fence, and the GPU never reaches it.
- **Measured:** the stuck submission waits on no copy or download fence, so no cross-queue cycle; WARP simply never finishes it.
- **Measured:** "sv - a view stepping sideways in front of a wall reports the step as every pixel's motion" alone, `--thorough`, hangs every time.
  It stops on the first frame that runs the full hybrid deferred pass, within 30 s.
  With that frame's `light_thin` dispatch removed, the hang moves to the first full hybrid forward frame.
  With `light_thin` cut to a constant store, it moves to the first wavefront frame.
  A single constant ray in `light_thin`, opaque and skipping procedurals, still hangs, and so does `light_thin` without any trace; a smaller wall changes nothing.
- **Measured:** at the hang one thread runs at 100 % on one instruction, `d3d10warp.dll+0x6796b` (WARP 10.0.26100.9711).
  Its bytes are a singly linked list walk, `next` at offset 0x10, that stops only at null or at the node it looks for; it never stops, so the list holds a cycle.
  A cycle in a driver's list is corruption: a node inserted twice or used after it was freed.
  An application causes that by calling into the driver concurrently where D3D12 asks for external synchronization.
- **Next:** name the function with Microsoft's public PDB for that build, and walk the thread's stack to the call of ours that enters it.
  The background pipeline builds that run while a frame is submitted are the first suspect.
- **For now:** the WARP leg of shaped-viewer-test is skipped (tests/dx12-entry.cc); undo that skip to reproduce.
