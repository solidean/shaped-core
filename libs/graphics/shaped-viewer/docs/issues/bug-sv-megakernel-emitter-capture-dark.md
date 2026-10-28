---
created: 2026-10-21
refs:
  - test: "sv - the wavefront tracer traces and denoises the viewer's image"
  - file: libs/graphics/shaped-viewer/tests/denoise-view-test.cc
  - file: docs/bugs-external/warp-raytracing-wrong-capture/readme.md
---

# Windows' WARP traces the Cornell box capture wrong about one run in eight

`denoise-view-test.cc`'s "the wavefront tracer traces and denoises the viewer's image" fails on WARP and never on hardware.
CI's Windows Clang job has hit it since 2026-10-21, and locally it fails about one run in eight under `--thorough --repeat`.
It is skipped on software adapters until WARP is fixed or CI runs a fixed one.

## What it is

A driver bug, as far as anything here can measure: [docs/bugs-external/warp-raytracing-wrong-capture](../../../../../docs/bugs-external/warp-raytracing-wrong-capture/readme.md) has the evidence.
In short, every input the trace reads matches a passing run when read back from the GPU, and WARP 1.0.21 from NuGet passes 80 runs where the system WARP fails one in eight.

The failing capture is the first wavefront capture of the run, always with a mean of 12.717 against the megakernel's 18.200.
Its image has the right camera and walls, both blocks missing, and a bright slanted surface across the scene.
CI once saw the megakernel's capture go dark instead (11.33 against 18.31), which is likely the same fault on the other tracer.

## Ruled out, by measurement

- Stale samples surviving a restart: the trace hash covers every instance's transform, BLAS and record, and the restarts were seen to happen.
- A BLAS built from a vertex buffer still streaming: the bytes the build consumed were read back and match.
- The emitter CDF, the instance records and the material parameter blocks: identical to a passing run.
- Uninitialized wavefront path state, and transient buffers read before written: restart clears the former, and a detector found none of the latter.

## What is left

The fault is inside WARP's acceleration-structure build or traversal, or something they read that no probe here covered.
Disabling sg's placement of transient buffers made this state vanish over 138 runs, and showed a milder one once, so placement or the timing it changes is part of the trigger.
Isolating it into a standalone `repro.cc` is what would let it be filed with Microsoft.

## Closing it

When the skip can go: either the system WARP on CI's image is fixed, or CI runs a fixed WARP ([feat-ci-nuget-warp](../../../../../docs/issues/feat-ci-nuget-warp.md)).
[bug-sv-warp-megakernel-quadric-flake](bug-sv-warp-megakernel-quadric-flake.md) is very likely the same fault and should be re-checked at the same time.
