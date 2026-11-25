# RADV: ACO's scheduler reorders VGPR spills to LDS, so a reload reads the wrong value

**Status:** open, root cause found in Mesa's source, minimized to a 24-line shader, not yet filed upstream; measured on 2026-11-25.
**Affects:** RADV in Mesa 26.2.0-devel (git-5c65d4fc31), an AMD Steam Machine (NAVI33, RDNA 3), SteamOS; upstream `main` has the same code.
**Found by:** `shaped-rendering-test`'s `sr - BC7 compression reproduces every two-subset partition and holds a gradient` on `sr vulkan / vulkan`, every run, max error 121.
**Tracked by:** [bug-sr-bc7-two-subset-radv-aco-scheduler](../../../libs/graphics/shaped-rendering/docs/issues/bug-sr-bc7-two-subset-radv-aco-scheduler.md).

The directory keeps the name it was filed under, from the BC7 encoder that found it; the encoder is not part of the reproduction any more.
[mesa-issue.md](mesa-issue.md) is the upstream issue, ready to paste, with what to attach.

## The cause

Since `133ef9f9` ("aco: spill VGPRs to LDS if it doesn't further limit occupancy", 2026-03-27), ACO spills VGPRs to LDS before scratch, in a compute shader whose workgroup is one wave.
The spill and reload are `ds_write_addtid_b32` and `ds_read_addtid_b32`, built with no `memory_sync_info`.
They are `aco_spill.cpp`'s `spill_vgpr` and `reload_vgpr`, lines 1398, 1413, 1461 and 1480 at 5c65d4fc31.
The scratch spills beside them carry `memory_sync_info(storage_vgpr_spill, semantic_private)`.
The pre-RA scheduler, which runs after the spiller, keeps two memory instructions in order only when their storage classes intersect (`aco_scheduler.cpp`, line 753).
An LDS spill's storage is empty, so it intersects nothing, and the scheduler moves LDS spills and reloads past each other freely, the same slot included.

The likely fix is giving the four `bld.ds` calls the scratch spills' `memory_sync_info`; it is unbuilt and untested here.

## What happens

`shader.hlsl` keeps 280 integers live through two passes, more than a lane's 256 VGPRs, in one wave of 64 threads.
`repro.cc` computes each thread's result on the CPU as well.
With the scheduler off, all 64 threads match; as compiled, all 64 are wrong, on every run.

In ACO's IR after register allocation (`RADV_DEBUG=shaders`), LDS slot 256 holds `%416` and is then reused for `%794`.
With `ACO_DEBUG=nosched`, `%416`'s reload comes before the reuse:

```
ds_write_addtid_b32 %1970:m0, %416:v[62] offset0:256
 v1: %1836:v[56] = ds_read_addtid_b32 %1970:m0 offset0:256
ds_write_addtid_b32 %1970:m0, %794:v[246] offset0:256
```

As compiled, the scheduler has moved the second spill above the reload, which then reads `%794`:

```
ds_write_addtid_b32 %1970:m0, %416:v[70] offset0:256
ds_write_addtid_b32 %1970:m0, %794:v[246] offset0:256
 v1: %1836:v[246] = ds_read_addtid_b32 %1970:m0 offset0:256
```

None of the shader's `ds_*_addtid` instructions prints a `storage:` field, where every scratch spill prints `storage:vgpr_spill semantics:private`.
The opposite direction happens too: in the BC7 encoder, a reload was hoisted above its own spill and read whatever the slot held before.
LDS outlives the workgroup, so such a read is decided by what ran earlier, which is why that encoder's wrong blocks were stable alone and varied run to run inside the test binary.

| configuration | threads wrong |
|---|---|
| as compiled | all 64 |
| `ACO_DEBUG=nosched` | none |
| `ACO_DEBUG=nosched,noopt` | none |
| `ACO_DEBUG=nosched-ilp`, `nosched-vopd`, `force-waitcnt`, `force-waitdeps` or `novn` | all 64 |

`ACO_DEBUG=validateir,validatera` reports nothing.

## The shader is tuned

Which instructions the scheduler moves depends on where the spiller puts them, so `N` is tuned: at 280 the reorder hits a slot, and at 275 or 285 it happens not to.
Every value is an integer and the result is exact, so the CPU check has no tolerance to argue about.
The shader was found by writing the pattern the BC7 encoder's IR showed, once its wrong values were traced to a stale LDS reload, rather than by cutting the encoder down.
That encoder, 2,452 lines of generated HLSL, is this entry's first version in git.

## Reproducing it

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>] [--prebuilt] [--ir <dir>]

It compiles `shader.hlsl` with the SDK's `dxc`, checks it with `spirv-val`, builds `repro.cc` with clang against the SDK, and runs it twice.
`shader.spv` is that compile, committed for a machine without DXC: `--prebuilt` runs it, and so does a machine with no SDK, building against the system's Vulkan.
`--ir <dir>` writes each arm's `RADV_DEBUG=shaders` output there, the attachments the issue wants.
Both runs set `RADV_DEBUG=nocache`; the second also sets `ACO_DEBUG=nosched`.
It exits 1 when threads are wrong only with the scheduler on, and 0 when every thread is right.
On the machine above it prints, the thread lists shortened:

```
as compiled:
    device: AMD Steam Machine (RADV NAVI33), vendor 1002, radv Mesa 26.2.0-devel (git-5c65d4fc31)
    wrong=0,1,2,…,63
ACO's scheduler off:
    device: AMD Steam Machine (RADV NAVI33), vendor 1002, radv Mesa 26.2.0-devel (git-5c65d4fc31)
    wrong=none
REPRODUCED: threads 0,1,2,…,63 are wrong as compiled, and every thread is right with ACO's scheduler off
```

## In the tree

Nothing works around it.
`bc-compress-test.cc` skips the partition test, and the BC7 half of `sr - BC1 and BC7 compress any mip level, a partial block and a single texel included`, by sg's known issue of this name.
The key is a vulkan context on the DXC route, on `radv` whose `driverInfo` starts `Mesa 26.2.0-devel`.
SGL's own SPIR-V of the encoder passes on that device, most likely because it spills differently, so the key leaves that route out.
A Mesa update past that version runs the tests again, and so does `uv run dev.py test --known-issues`.

## Re-checking

On a Mesa update, run `run.py`; an exit of 0 retires this entry, its issue and the skip.
An exit of 1 on a new version wants that version added to the entry's key in `known_issues.cc`.
An exit of 0 on a Mesa that still has the bug is possible, since the shader is tuned: before retiring the entry, check `aco_spill.cpp`'s LDS spills for a `memory_sync_info`.
