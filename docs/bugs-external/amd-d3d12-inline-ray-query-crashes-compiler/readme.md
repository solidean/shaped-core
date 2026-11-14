# AMD: an unsigned ray query crashes the driver's compiler once an unsigned plain shader is in its cache

**Status:** open, reproduced standalone, not yet filed upstream, worked around in SGL's DXIL writer; measured on 2026-11-13.
**Affects:** AMD D3D12 driver 32.0.21042.62 (`amdxc64.dll`), the Radeon iGPU of a Ryzen 9 9950X3D2 (RDNA 2), Windows 11, the system runtime.
**Found by:** the differential corpus under `SC_GPU=amd`: every inline trace of SGL's native DXIL ended the process, sg's own trace test included.

## What happens

`CreateComputePipelineState` on a container that traces inline crashes with an access violation inside `amdxc64.dll`.
It never returns an error; the process ends inside the driver's deserializer (`boost::archive ... load_object_data`).
It happens only when all of these hold:
- The container is **unsigned**: its digest is the sixteen bytes of `0x01` the runtime accepts from a container no validator signed.
- Its shader uses **RayQuery**.
- The driver's shader cache, `%LOCALAPPDATA%\AMD\DxcCache`, already holds an entry an **unsigned plain shader** left, from this process or any earlier one.

## The trigger

Each row runs its containers in that order, each in a fresh process, starting from a reset cache.

| sequence | AMD | Intel Arc B580 |
|---|---|---|
| ray query, unsigned, alone | compiled | compiled |
| plain shader unsigned, then ray query unsigned | **crashed** | compiled |
| plain shader unsigned, then ray query signed | compiled | compiled |
| ray query unsigned, then plain shader unsigned | compiled | — |

The unsigned container differs from the signed one in its sixteen digest bytes alone.
The driver seems to key a cache entry by the container's digest, which every unsigned container shares.
So a ray query meets the entry a plain shader left, and its compiler crashes reading it.
Once such an entry is there, every unsigned ray query crashes until the cache is reset, in every later process.

How it was narrowed:
- SGL's own HLSL for the crashing entry point, compiled by DXC and signed, compiles; SGL's native DXIL of it crashes.
- That DXIL passes `dxil.dll`'s validator, and signed by it, compiles.
- So the unsigned digest decides, and a DXC module stamped with it crashes the same way.
- An old cache set aside made the crash go away, and bisecting it found one family of cache files, the one the unsigned digest writes.
- A plain unsigned shader then a ray query, in a reset cache, re-creates that state from nothing.

The D3D12 debug layer, the shader model (6.5 to 6.8), embedded debug info and the DXC version change nothing.

## Why it is not our bug

An unsigned container is legal: the runtime accepts its digest by design, and the plain shader and the ray query are each valid DXIL.
Compiling them in one order rather than the other must not crash the driver, and a cache must not hand one shader's entry to another.

## Run it

```bash
uv run run.py [--dxc <dxc.exe>]
```

`run.py` builds [repro.cc](repro.cc), compiles [shaders/plain.hlsl](shaders/plain.hlsl) and [shaders/ray_query.hlsl](shaders/ray_query.hlsl), and stamps the unsigned digest on copies.
It runs the three sequences of the table and exits 1 when the crash reproduces and both controls compile.
It sets `DxcCache` aside for the run where it can; where another process holds it open, reset it first in AMD Software (Graphics, Advanced, Reset Shader Cache).
A run whose first control crashes says so: the cache already held a plain shader's entry.

## In shaped-core

SGL's native DXIL writer was the one producer here that shipped unsigned containers, so every native shader on that driver shared one cache entry.
It now signs them: the digest is DXBC's checksum, which `sgl::ir::dxbc_digest` computes as dxil.dll does, so each container has its own.
With that, an inline trace of native DXIL creates on that driver whatever the cache holds.
To retire this entry, run `run.py` after a driver update; once it reports nothing, the signing stays anyway, since it costs nothing.

[amd-d3d12-dxil-without-llvm-ident-crashes](../amd-d3d12-dxil-without-llvm-ident-crashes/readme.md) used unsigned containers too, on this same driver.
Its finding may be this one: its variants that crashed were unsigned, and the cache's state then is not known.
