# AMD Vulkan: a ray-tracing pipeline traces black or hangs after some other pipelines traced, from the modules it traces right alone

**Status:** not reduced to a standalone repro; reproduced in shaped-viewer's own test, every run; not yet filed upstream.
**Affects:** an AMD Radeon iGPU (RDNA 2), AMD's proprietary Vulkan driver 2.0.353 (2026.04.17), Windows 11.
RADV on the same chip generation traces it right, and so do the Intel Arc B580's Vulkan driver and AMD's own D3D12 driver.
**Found by:** `sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan`, whose cutout case first drew 32 pixels wrong, then nothing.
The SGL-side history is shaped-graphics-language's issue bug-sgl-spirv-cutout-trace-differs-from-dxc-on-amd.

## What happens

The test traces a scene per case through one resource manager, each case through its own ray-tracing pipeline.
The cutout case is a floor, a box and a panel whose per-vertex opacity an any-hit tests stochastically.
Traced alone it agrees with DXC's route to 1e-6 relative.
Traced after certain other cases, it comes back all zeros, and most runs the device is lost a few seconds later:

```
traced first, then cutout                      AMD Vulkan              Arc B580 Vulkan   AMD D3D12
---------------------------------------------  ----------------------  ----------------  ---------
nothing, pbr, openpbr constants, cutout        traces                  traces            traces
unlit, mesh attributes                         black, or device lost   traces            traces
unlit, then pbr, textured, quadrics, openpbr   traces                  traces            traces
```

SGL's SPIR-V and DXC's fail alike, whichever traces first.

## Why it is probably not our bug

Every hit stage of the cutout pipeline is byte for byte the same in the run that traces and in the run that fails, on both routes.
So is the scene; only what the process traced before differs.
Ruled out, each by an experiment in the test:

- the alpha-cutoff box and the quadrics: the floor, a plain box and the panel suffice;
- shaped-graphics' persistent pipeline tier, off under `SC_SG_COLD=pipelines`;
- the previous case's resources being released and their bindless slots reused, since keeping them alive changes nothing;
- the `StorageBufferArrayNonUniformIndexing` capability neither route declares, patched into every module;
- standard, synchronization and GPU-assisted validation, which report nothing before the loss.

What is left is state the driver keeps between pipelines, such as a cache of compiled hit shaders.
That is a hypothesis: a standalone repro would replay the pipelines from their modules, which needs the viewer's descriptor layout and scene, and does not exist yet.

## Reproducing it

    SC_GPU=amd uv run dev.py test "sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan" --thorough --known-issues

`--known-issues` runs it through the skip below; it fails within about 8 s once the shaders are cached.

## What we do about it in the tree

The known issue `amd-vulkan-cutout-trace-after-another-hangs` in shaped-graphics' `known_issues.cc` covers AMD's proprietary Vulkan driver.
`cross-backend-trace-test.cc` skips its vulkan halves by it; the DXIL half still runs on AMD.
No workaround exists, since what the driver keeps is not known.

## What to re-check after a driver update

Run the command above; once it passes, the known issue may go.
