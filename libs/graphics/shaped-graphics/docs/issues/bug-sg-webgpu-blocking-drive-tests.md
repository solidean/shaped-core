---
created: 2026-11-19
refs:
  - test: sg - limits report the portable floors
  - file: libs/base/clean-core/src/clean-core/thread/async.cc
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-19T13:34Z
---

# Some sg tests fail on emscripten-webgpu: a blocking drive cannot progress

`uv run dev.py test shaped-graphics-test --preset emscripten-webgpu-relwithdebinfo` fails under node within its first seconds, and `--fail-grace` then skips 416 tests.
Each failing test logs `check failed: false — a blocking drive cannot progress: the graph waits on something no scheduler or pump here will run` (`async.cc:279`), then that it left async work running.

Measured on 2026-11-19, at `ccfd9f654` with no local changes:
- `uv run dev.py test "sg - limits report the portable floors" --preset emscripten-webgpu-relwithdebinfo` fails alone, so this is no ordering effect.
- Others among the first to fail: `sg - a BC texture is refused where texture_compression_bc is absent`, `sg - a declared group index reaches the group layout`,
  and `sg - dropping a list cancels the downloads it recorded`.
- `"sg - a geometry stage"` passes on the same build, so not every test of the binary is affected.

Hypothesis, unchecked: these tests drive a future to completion by blocking, which an unthreaded wasm build cannot pump.

## Cause

The driver's `finish_each`, `sg_test::settled`, first awaits `ctx->routines.idle_completion()`.
That was always a cold coroutine homed to the device thread, ready only once it had ticked, even with nothing to do.
On webgpu the device thread is the main thread, inside whose home every test body runs.
nexus drives a sync child's `finish_each` with `cc::try_async_blocking_get`, which cannot run main-homed work from inside main, and an unthreaded build has no other thread: the drive asserted.
Async children were never affected, since nexus starts their `finish_each` as a node.

`idle_completion` is now ready at once where every routine is up or failed at the current reload generation, which `sg - an idle registry's idle_completion is ready when asked` pins.
The whole binary then runs on emscripten-webgpu: 670 of 671 tests pass, the last being `bug-sg-webgpu-sampler-ignores-mip-lod-bias`, which the deadlock's `--fail-grace` had been skipping.
A sync test that leaves a routine pending would still deadlock there; none does today.
