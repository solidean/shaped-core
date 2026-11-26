# D3D12: a mesh pipeline stated without a topology refuses its own cached blob

**Status:** reproduced standalone, not yet filed upstream; sg's dx12 backend states a triangle mesh stage's topology.
**Affects:** the D3D12 runtime on Windows 11: WARP, an Intel Arc B580 (driver 32.0.101.9150) and an AMD Radeon iGPU (32.0.21042.62) on the system runtime.
In sg the AMD iGPU refused it under the Agility SDK 1.620 as well, and an RTX 4090 (driver 32.0.16.1088) did before this repro existed.
**Found by:** every dx12 mesh-shaded pipeline's restore from the persistent tier being refused, run after run (bug-sg-dx12-mesh-pipeline-blob-refused).

## What happens

`repro.cc` builds each pipeline through a pipeline-state stream, takes its `GetCachedBlob`, and builds the same stream again with the blob as its `CACHED_PSO`.

```
pipeline stream                                    WARP          Arc B580      AMD iGPU
-------------------------------------------------  ------------  ------------  ------------
root signature, vertex stage, triangle topology    taken         taken         taken
root signature, mesh stage                         E_INVALIDARG  E_INVALIDARG  E_INVALIDARG
root signature, mesh stage, triangle topology      taken         taken         taken
```

The debug layer names it "a cached PSO which does not match the rest of the description provided".
A topology stated as `UNDEFINED` is refused the same way, and so is the cached PSO placed first in the stream.
An empty amplification subobject beside the mesh stage changes nothing.

A mesh pipeline's topology is the mesh stage's own `outputtopology`, so the stream needs none.
Where it states one, the debug layer requires that it match the stage's: a triangle topology over a stage of lines is an error of its own.

## Why it is not our bug

`repro.cc` is D3D12 and the C++ standard library, and the restore hands back the blob the runtime just made, to an identical stream.
Every adapter refuses it alike, WARP among them, so it is the runtime's rather than a driver's.

## Reproducing it

    uv run run.py

It runs on WARP and on every hardware adapter with mesh shaders.
It exits 1 when some adapter refuses the bare mesh stage's blob and takes the other two, 0 when every adapter takes all three, and 2 when the run is inconclusive.

## What we do about it in the tree

`dx12_raster_pipeline.cc` reads the mesh stage's output topology from its container's `PSV0` part, and states it in the stream where it is triangles.
Such a pipeline restores from a cached blob and hands one out; a mesh stage of lines, or one whose `PSV0` says nothing readable, does neither.
`sg - a mesh-shaded pipeline of triangles built from its own cached blob takes it, and draws as before` holds it.

An Intel GPU restores no mesh-shaded blob either, measured on the Arc B580 and not reduced.
Once that device had restored some, a later mesh pipeline drew nothing now and then, blob or none, in a full run of sg's mesh tests; AMD drew every one.

## What to re-check after a runtime update

Run `uv run run.py`; once it exits 0, the stated topology may go, and with it the `PSV0` read and the skip for lines.
