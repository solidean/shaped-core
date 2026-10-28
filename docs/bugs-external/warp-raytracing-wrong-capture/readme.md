# WARP: a path-traced capture comes out with the wrong geometry, about one run in eight

**Status:** open, not yet filed upstream; the trigger is not isolated, so there is no `repro.cc` yet.
Its test is skipped on software adapters in shaped-viewer.
**Affects:** the WARP that ships with Windows, `d3d10warp.dll` 10.0.26100.9711, on x64; CI's Windows Clang job and a local Windows 11 machine, October 2026.
WARP 1.0.21 from the `Microsoft.Direct3D.WARP` NuGet package does not show it.
**Found by:** sv's "the wavefront tracer traces and denoises the viewer's image" failing on WARP and never on hardware.
Its issue is [bug-sv-megakernel-emitter-capture-dark](../../../libs/graphics/shaped-viewer/docs/issues/bug-sv-megakernel-emitter-capture-dark.md).

## What happens

The test captures a Cornell box lit by an emissive lamp five times, each through a new `sv::interactive`: once with the megakernel, then four times with the wavefront tracer.
About one run in eight, the first wavefront capture is wrong.
Its image keeps the camera and the side walls, but both blocks are missing and a large bright surface slants from the lamp's edge toward the scene's centre.
Its mean is 12.717 against the megakernel's 18.200, the same value to the last digit in every failing run, so it is one fixed wrong state rather than noise.
The other three wavefront captures of the same run are right.

## Why it is the driver and not us

Every input the trace reads was compared between a failing and a passing WARP run, read back from the GPU:

| what | where it was read | failing vs passing |
|---|---|---|
| each mesh's vertex buffer | on the frame's own command list, right after `build_blas` consumed it | identical |
| the same buffers, and the lamp's emitter CDF | when the capture is written | identical |
| the instance records, bindless indices included | every traced frame | identical |
| every material parameter block, as uploaded | every traced frame | identical |
| restart hash, accumulation counts, stand-ins, pending emitters and meshes | every traced frame | the same sequence |
| when each BLAS and the CDF are built, relative to the frames | every capture | the same |

No transient buffer is read before its frame writes it: a detector in the dx12 command list's access tracking found none.
The driver is the variable that decides it:

| WARP | runs | failures |
|---|---|---|
| system, 10.0.26100.9711 | several hundred over the session | about one in eight, first seen at runs 1 to 23 of each batch |
| NuGet 1.0.21, beside the test binary | 80 | none |

Against one in eight, 80 passes in a row is about a 2·10⁻⁵ chance.
Microsoft's changelogs for the NuGet WARP list several ray-tracing fixes since the code Windows ships.
Among them: a partial rewrite with JIT-compiled traversal (1.0.14), empty BLAS inputs (1.0.15), a larger traversal stack (1.0.17), and wave-uniform data in recursive invocations.
None names this symptom.

One more variable moves it, which the trigger has to explain once it is found.
With sg's transient buffers made dedicated allocations rather than placed in the per-epoch bump heap, the 12.717 state did not occur in 138 runs.
A different, milder failure did, once: the right geometry at 16.86, a darker floor.
Placement changes both memory reuse and timing, so this does not say which of the two the driver is sensitive to.

## Reproducing it

Not standalone yet.
In the tree, with the skip in `denoise-view-test.cc` reverted:

```raw
uv run dev.py test "sv - the wavefront tracer traces and denoises the viewer's image" --thorough --repeat 40
```

On x64 Windows with the system WARP it fails within a few dozen runs.
With `d3d10warp.dll` from `Microsoft.Direct3D.WARP` 1.0.21 (`build/native/bin/x64/`) copied next to `shaped-viewer-test.exe`, it does not.

## What shaped-core does about it

The test skips on any software adapter, `TEMPORARY(bug-sv-megakernel-emitter-capture-dark)` in `libs/graphics/shaped-viewer/tests/denoise-view-test.cc`.
`sg::adapter_info::driver_version` is opaque by contract and reports the D3D runtime rather than the WARP DLL anyway, so the skip cannot be narrowed to the broken versions.
[bug-sv-warp-megakernel-quadric-flake](../../../libs/graphics/shaped-viewer/docs/issues/bug-sv-warp-megakernel-quadric-flake.md) is very likely the same fault.
It is two quadric tests whose images come out wrong on CI's WARP and never alone, skipped the same way.
Running CI's WARP legs on the NuGet WARP instead is [feat-ci-nuget-warp](../../issues/feat-ci-nuget-warp.md).

## Re-checking after an update

Run the command above with the skip reverted, on the system WARP after a Windows update, or on a newer NuGet WARP.
Forty passes in a row says this WARP is fixed; if the system one is fixed on CI's image too, both skips can go.
