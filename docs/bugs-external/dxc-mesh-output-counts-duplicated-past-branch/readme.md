# DXC: jump threading copies a mesh shader's one SetMeshOutputCounts, and its own validator refuses the copies

**Status:** open, not yet filed upstream; one test shader is written around it, and SGL's HLSL does nothing about it.
**Affects:** DXC 1.9.2602.24 (shaped-core's fetched dxc), 1.9.2602.17 (Windows SDK 10.0.28000.0) and 1.9.0.5512 (Vulkan SDK 1.4.363.1), DXIL at `ms_6_6` and above.
**Found by:** `halves` in shaped-graphics' `tests/shaders/sgl/meshes.sgl`, once it held a barrier under a branch on its workgroup id ahead of `set_counts`.

## What happens

A mesh shader calls `SetMeshOutputCounts` once, in uniform control flow, between two branches.
The first tests `group.x == 1`, and the second `group.x == 0`, where `group` is the signed `int3` of `SV_GroupID`.
On the first branch's taken path the second's outcome is known, so the optimizer threads it, and the block that holds the call is copied into that path:

```
; <label>:3
  call void @dx.op.barrier(i32 80, i32 10)
  call void @dx.op.setMeshOutputCounts(i32 168, i32 3, i32 1)
  br label %6
; <label>:4
  call void @dx.op.setMeshOutputCounts(i32 168, i32 3, i32 1)
```

DXC's validator then refuses its own output:

```
error: SetMeshOUtputCounts cannot be called multiple times.
error: Non-Dominating SetMeshOutputCounts call.
```

## Why it is not our bug

The HLSL is valid, and calls `SetMeshOutputCounts` exactly once on every path, where every invocation reaches it.
`shader.hlsl` is the whole reproduction, with no include, and every ingredient is a macro that `run.py` varies:

| row | result |
|---|---|
| as written, `ms_6_6 -O3` | refused |
| `ms_6_9 -O3`, `ms_6_6 -O1` | refused |
| `ms_6_5 -O3` | compiles, one call in the DXIL |
| `ms_6_6 -Od` | compiles |
| no branch before the call | compiles |
| the first branch on `group.y` | compiles |
| the first branch tests 5, not 1 | refused |
| `uint`, not `int` | compiles, one call in the DXIL |

All three DXC builds give the same table.
So the trigger is a branch whose outcome decides the second one's, with the call between them, at shader model 6.6 or above under optimization.
The signed comparison is part of it: on the `uint` of `SV_GroupID` itself the same shader keeps one call, and SGL's HLSL converts the workgroup id to `int3`.

## Reproducing it

    uv run run.py

It finds `$DXC`, shaped-core's fetched dxc, the newest Windows SDK's and the one on `PATH`, and prints the table for each.
It exits 1 when any of them refuses the shader as written, and 0 when none does.

## What we do about it in the tree

SGL's HLSL is left as it is, since it is correct and the bug is in DXC's optimizer.
SGL's own DXIL and DXC's SPIR-V draw the same shader, so dx12 through DXC is the one route that refuses it.
`halves` and `culled_column` in [meshes.sgl](../../../libs/graphics/shaped-graphics/tests/shaders/sgl/meshes.sgl) hold their barrier after `set_counts`, where the call already dominates the branch.
A user's mesh stage written the other way round fails its pipeline's build on dx12 through DXC with this message.

## What to re-check after a DXC update

Run `uv run run.py`; once it exits 0, the barrier in `meshes.sgl` may move back ahead of `set_counts`.
