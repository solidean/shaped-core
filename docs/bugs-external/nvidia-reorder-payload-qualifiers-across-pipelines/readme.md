# NVIDIA: a reordering raygen loses its payload beside a pipeline of the other qualifier setting

**Status:** open, reproduced standalone, not yet filed upstream, worked around in SGL's DXIL writer; measured on 2026-11-09.
**Affects:** NVIDIA D3D12 driver 621.01, GeForce RTX 5070 Ti, Windows 11; the Agility SDK 1.620.1 at Shader Model 6.9, DXC 1.9.2602.24.
**Found by:** `sg - an SGL raygen that reorders its invocations traces what it would have traced` in shaped-graphics-test.
There it failed on one of the two dx12 contexts in every run, and which one moved between runs.

## What happens

Two ray-tracing pipelines are built from one HLSL file on one device.
The first is compiled with payload access qualifiers and the second with DXC's `-disable-payload-qualifiers`, or the other way round.
Their raygens call `dx::MaybeReorderThread` before `TraceRay`.

The pipeline created second loses what its closest hit writes into the payload.
Its raygen reads back the value it stored before the trace, as if the closest hit had never run.
The first pipeline is correct, the calls all succeed, and the debug layer says nothing.

## The trigger

`run.py` runs each arm as a process of its own; a word of 10 is right and 0 is the loss.

| first pipeline | second pipeline | device | raygen | second pipeline's words |
|---|---|---|---|---|
| qualified | unqualified | shared | reorders | **0**, every ray |
| unqualified | qualified | shared | reorders | **0**, every ray |
| qualified | qualified | shared | reorders | 10 |
| unqualified | unqualified | shared | reorders | 10 |
| qualified | unqualified | one each | reorders | 10 |
| qualified | unqualified | shared | no reorder | 10 |

So the bug needs all three:

- the two pipelines disagreeing on payload access qualifiers;
- one device, which the Agility SDK's device factory gives only when the second pipeline is created on the first device;
- a raygen that reorders.

A payload of one field, read and written by the caller and the closest hit, is enough.
Which fields a larger payload loses depends on its shape, and not simply.
SGL's seven-field `radiance` lost `instance_id` and `is_lit` while `t` and `is_hit` arrived.
Its `instance_id` beside `is_hit` alone, the one field the miss writes too, lost nothing.
In sg's test the loss showed differently again: the failing raygen wrote nothing at all into its result buffers.
Setting both of sg's routes to the same qualifier setting fixed that test too, five runs of five, so it is the same bug.

## Why it is not our bug

`shader.hlsl` is 40 lines compiled by stock DXC, and `repro.cc` builds the pipelines with plain D3D12.
Either qualifier setting is valid on its own, and each pipeline is correct alone or on a device of its own.
Whether a payload carries qualifiers is a property of each state object.
A state object that reads another one's choice through the device they share is the driver's to fix.

## Reproducing it

    uv run run.py [--agility <install>] [--dxc-dir <dir of dxcompiler.dll>] [--vendor <hex PCI id>]

It builds `repro.cc` with clang-cl against the Agility SDK's headers, which it finds in this repository's `extern/agility/.install` unless told otherwise.
`repro.cc` compiles `shader.hlsl` at run time through a `dxcompiler.dll` that knows Shader Model 6.9, this repository's `extern/dxc/.install/bin` by default.
It exits 1 when both mixed arms lose the payload and every control keeps it, 0 when nothing is lost, and 2 when the run is inconclusive.
On the machine above:

```
qualified, then unqualified    exit 1  second pipeline: |0 0 0 0 0 0 0 0 |0 0 0 0 0 0 0 0 |0 0 0
unqualified, then qualified    exit 1  second pipeline: |0 0 0 0 0 0 0 0 |0 0 0 0 0 0 0 0 |0 0 0
both qualified                 exit 0  second pipeline: |10 10 10 10 10 10 10 10 |10 10 10 10 10
neither qualified              exit 0  second pipeline: |10 10 10 10 10 10 10 10 |10 10 10 10 10
mixed, a device each           exit 0  second pipeline: |10 10 10 10 10 10 10 10 |10 10 10 10 10
mixed, no reordering           exit 0  second pipeline: |10 10 10 10 10 10 10 10 |10 10 10 10 10
REPRODUCED: beside a pipeline of the other qualifier setting on one device, a reordering raygen loses its closest hit's payload write
```

## How shaped-core met it, and what we do about it

DXC writes `!dx.dxrPayloadAnnotations` for every `[raypayload]` struct, and SGL's native DXIL writer wrote none.
So sg's dx12 context on DXC's route and its native-DXIL context built pipelines of the two settings.
shaped-graphics-test brings both up on one adapter, and D3D12 gives them one device.
[bug-sgl-dxil-reordered-raygen-wrong-cell](../../../libs/graphics/shaped-graphics-language/docs/issues/.closed/2026/11/bug-sgl-dxil-reordered-raygen-wrong-cell.md) holds the narrowing.

SGL's native writer now states every payload's access as DXC does for SGL's text (EMIT-137), so the two routes agree.
[writer_stage.cc](../../../libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer_stage.cc) writes it and names this directory.
`slib - SGL's DXIL states each payload field's access, as DXC does for SGL's text` holds it there.
The annotations are what DXC writes anyway, so they stay when NVIDIA fixes the driver.

An application still meets it with a single route: two of its pipelines need only disagree, as code from two compilers or compiled with two flag sets would.

## What to re-check

On a driver update, run `run.py` on an NVIDIA GPU.
Exit 0 retires this entry; the annotations SGL writes stay either way.
