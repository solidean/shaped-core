# The Mesa issue, ready to file

File at <https://gitlab.freedesktop.org/mesa/mesa/-/issues/new>, with the labels `RADV` and `ACO`.
Attach `shader.hlsl`, `shader.spv`, `repro.cc` and `run.py` from this directory.
Attach the two IR dumps too, which `uv run run.py --ir <dir>` writes as `aco-as-compiled.txt` and `aco-nosched.txt`, about 300 KB each.
Everything below the line is the issue's body; the title is:

    aco: LDS VGPR spills have no memory_sync_info, so the scheduler reorders a reload past a spill to its slot

---

### System information

- OS: SteamOS, kernel 7.2.7-valve2
- GPU: AMD Steam Machine (RADV NAVI33), PCI 1002:7481, RDNA 3
- Mesa: 26.2.0-devel (git-5c65d4fc31)
- Shader compiler: DXC 1.9.0.5399 (Vulkan SDK 1.4.357.1), `-T cs_6_8 -O3 -spirv -fspv-target-env=vulkan1.3`

### Describe the issue

A compute shader that spills VGPRs computes wrong results on every thread.
With `ACO_DEBUG=nosched` every thread is right.

Since 133ef9f9 ("aco: spill VGPRs to LDS if it doesn't further limit occupancy"), a single-wave compute shader spills VGPRs to LDS with `ds_write_addtid_b32` / `ds_read_addtid_b32`.
In `aco_spill.cpp`, `spill_vgpr` and `reload_vgpr` build those instructions with no `memory_sync_info` (lines 1398, 1413, 1461 and 1480 at 5c65d4fc31).
The scratch spills beside them carry `memory_sync_info(storage_vgpr_spill, semantic_private)`.
The pre-RA scheduler keeps two memory instructions in order only when their storage classes intersect (`aco_scheduler.cpp`, line 753).
An LDS spill's storage is empty, so the scheduler moves LDS spills and reloads past each other freely, including ones to the same slot.

In the IR after RA, slot 256 holds `%416` and is then reused for `%794`.
With `ACO_DEBUG=nosched` the reload of `%416` comes before the reuse:

```
ds_write_addtid_b32 %1970:m0, %416:v[62] offset0:256
 v1: %1836:v[56] = ds_read_addtid_b32 %1970:m0 offset0:256
ds_write_addtid_b32 %1970:m0, %794:v[246] offset0:256
```

As compiled, the second spill has moved above the reload, which then reads `%794`:

```
ds_write_addtid_b32 %1970:m0, %416:v[70] offset0:256
ds_write_addtid_b32 %1970:m0, %794:v[246] offset0:256
 v1: %1836:v[246] = ds_read_addtid_b32 %1970:m0 offset0:256
```

None of the `ds_*_addtid` instructions prints a `storage:` field, while every scratch spill prints `storage:vgpr_spill semantics:private`.
In a larger shader (a BC7 encoder) the other direction happens too: a reload is hoisted above its own spill and reads whatever the slot held before.

The likely fix is giving the four `bld.ds` calls the scratch spills' `memory_sync_info`.
I have not built or tested that.

### Regression

Most likely since 133ef9f9, from reading the source; not bisected.
Current `main` has the same code.

### Steps to reproduce

The shader keeps 280 integers live through two passes, more than the 256 VGPRs a lane has, in one wave of 64 threads:

```hlsl
[[vk::binding(0, 0)]] StructuredBuffer<uint> input;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> output;
#define N 280
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint t = id.x;
    uint v[N];
    [unroll] for (uint i = 0; i < N; ++i) v[i] = input[i];
    for (uint m = 0; m < 2; ++m)
    {
        [unroll] for (uint i = 0; i < N; ++i) v[i] = v[i] * 3 + (t ^ v[(i * 7) % N]);
    }
    uint acc = t;
    [unroll] for (uint i = 0; i < N; ++i) acc ^= v[i];
    output[t] = acc;
}
```

`repro.cc` fills `input[i] = i * 0x9E3779B9 + 1`, dispatches one workgroup, and compares each thread's result with the same computation on the CPU.
The arithmetic is integer, so the comparison is exact; every value is initialized and every index is in bounds.

    uv run run.py              # DXC from the Vulkan SDK, or the system's Vulkan and shader.spv without one
    uv run run.py --prebuilt   # the attached shader.spv, no DXC needed

Or by hand: `c++ -O2 -std=c++20 repro.cc -o repro -lvulkan`, then `RADV_DEBUG=nocache ./repro --spirv shader.spv`, with and without `ACO_DEBUG=nosched`.

`N` is tuned: at 280 the reorder hits a slot, and at 275 or 285 it happens not to.

### Expected behavior

All 64 threads match the CPU, as with `ACO_DEBUG=nosched`.

### Actual behavior

All 64 threads are wrong, on every run.

| configuration | threads wrong |
|---|---|
| as compiled | all 64 |
| `ACO_DEBUG=nosched` | none |
| `ACO_DEBUG=nosched,noopt` | none |
| `ACO_DEBUG=nosched-ilp`, `nosched-vopd`, `force-waitcnt`, `force-waitdeps` or `novn` | all 64 |

`ACO_DEBUG=validateir,validatera` reports nothing.

### Log files

`aco-as-compiled.txt` and `aco-nosched.txt` are `RADV_DEBUG=nocache,shaders` output for the two arms.
