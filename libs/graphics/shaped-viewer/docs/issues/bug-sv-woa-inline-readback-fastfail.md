---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/tests/openpbr-bsdf-test.cc
  - file: libs/graphics/shaped-viewer/tests/volumetric-furnace-test.cc
  - file: libs/graphics/shaped-viewer/tests/pathtraced-view-test.cc
  - file: libs/graphics/shaped-viewer/tests/quadric-trace-test.cc
  - file: libs/graphics/shaped-viewer/tests/decal-trace-test.cc
  - file: libs/graphics/shaped-viewer/tests/pt-support-probe-test.cc
  - file: libs/graphics/shaped-viewer/tests/runtime-probe-test.cc
  - test: shaped-viewer-test
---

# The inline readback path fastfails on Windows on ARM

Every sv test that reads back through `cmd.download` is skipped there, under `CC_ARCH_ARM64 && _WIN32`.
That is `openpbr-bsdf-test` and `volumetric-furnace-test` first, then the probes, the quadric and decal traces and the light tests.
[structure.md](../structure.md) had already noted that path was never exercised here, so this was the first time sv asked for an inline readback at all.

## What happens

`shaped-viewer-test` exits `0xC0000409` with no output whatsoever, roughly seven seconds in.
Bisected with flushed markers to the advance-and-drain (now a blocking get on `ctx.idle_completion()`), on the probe's ECHO dispatch.
That dispatch is 32 threads reading one struct and writing four floats, and its command list also recorded a `cmd.download`.
The workload is therefore not the cause, and neither is the closure.

## What it is not

Not an assertion: nexus installs clean-core's crash handler, which hooks an SEH filter and `SIGABRT` and prints the running test, and `abort()` raises `SIGABRT`.
An earlier failure in the same binary proved that path works, arriving as a clean message.
Not a lost device: the wait was wrapped in a `catch (sg::device_lost_exception const&)` that never fired.
A `__fastfail` bypasses both handlers and loses buffered output, which is why this reads as a silent exit — and `/GS` uses exactly that for a genuine stack-cookie failure.

## Where to look, with hardware

The obvious first move is `ctx.download` instead of `cmd.download`.
sg documents the context-level one for a bulk, off-the-frame-path read, which is what both tests actually want.
It would say whether the inline staging ring is the part that breaks.

`libs/base/clean-core/tests/record/record-async-scope-test.cc` records a separate MSVC ARM64 defect around coroutine resume.
The completion machinery the epoch advance waits on is that same `cc::async`, so it is worth reading before assuming the two are unrelated.
sg's own `transfer/download-async-test.cc` passes on that leg, so whatever this is, it needs the combination rather than the download alone.
