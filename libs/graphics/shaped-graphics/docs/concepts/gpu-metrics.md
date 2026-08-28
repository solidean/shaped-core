# GPU metrics

How much of the GPU's memory this process may use, and how busy the device is.

## Why this is in sg rather than in cc

Every other resource a program wants to watch — CPU, memory, disk, network — comes from a documented OS call, so it
lives in clean-core's platform layer.

A GPU does not.
There is **no portable "how busy is it" syscall anywhere**, and even the memory figures come from the graphics API
rather than from the operating system.
Putting them in cc would mean the bottom of the stack loading vendor libraries and knowing about adapters, which is a
category error for the library whose neighbours are `console.hh` and `environment.hh`.

So sg owns them outright and cc never mentions GPUs at all.
What cc does provide is the stamp seam sg contributes through — see below.

## Two memory figures, and they are not the same scale

**`adapter_info::dedicated_video_memory_bytes` is what the board has.**
A property of the hardware, so it sits with the adapter description rather than with the live readings.

**`gpu_memory_usage::budget_bytes` is what this process may use right now.**
It shrinks as other processes take memory, and it is what the OS will actually let this program keep resident.

On an otherwise idle machine with an RTX 4090 those read 24138 MiB and 23370 MiB.
**Dividing one by the other is wrong**, and it is the mistake that makes a dashboard report a card as 20% full while
the driver is paging.
Draw usage against the budget; show the board size as a separate fact.

```cpp
auto const& adapter = ctx.adapter();
adapter.dedicated_video_memory_bytes;              // optional<i64> — the board; 0 is real on an integrated GPU

if (auto const memory = ctx.query_gpu_memory(); memory.has_value())
{
    memory.value().budget_bytes;                   // what this process may use now
    memory.value().current_usage_bytes;            // what it is using against that
}
```

## What each backend reads

**dx12** takes the board size from `DXGI_ADAPTER_DESC1::DedicatedVideoMemory`, and the live figures from
`IDXGIAdapter3::QueryVideoMemoryInfo` on the **LOCAL** segment.
`NON_LOCAL` is system memory the GPU reaches across the bus; adding it reports a budget nothing can keep resident.
The context retains its `IDXGIAdapter3` for this, because re-obtaining it per query would cost a COM round trip on a
call a dashboard makes every frame.

**vulkan** sums the `DEVICE_LOCAL` heaps for the board size, and reads `VK_EXT_memory_budget` for the live figures.
Without that extension vulkan reports only how big the heaps *are*, which says nothing about what is left, so the query
refuses rather than passing a static number off as a reading.

## Load is declared and refuses everywhere

`query_gpu_load` exists on `sg::context` and returns an error on every backend today.

Neither D3D12 nor Vulkan exposes utilization at all.
The routes are OS-level or per-vendor:

- **Windows** — `D3DKMTQueryStatistics`, which is what a task manager uses.
- **Linux** — `/sys/class/drm/card*/device/gpu_busy_percent`, which amdgpu provides and i915 does not.
- **macOS** — IOKit `PerformanceStatistics`.

None is implemented.
The API exists so that adding one later moves no call site, and **a refusal beats a zero**: a zero draws as an idle GPU
on a machine that is pinned.

## Stamping a recording

sg registers an `sg.gpu` stamp contributor when a context is created, so a `cc::rec` recording says which adapter the
run used — name, ids, driver version, board memory.
That is how a crash report identifies the GPU while cc goes on knowing nothing about GPUs.

The section describes the **first** context's adapter: a stamp provider hands back a span, so the bytes must outlive
the call, and a program creating several contexts would otherwise rewrite a buffer a recorder is reading.

See [clean-core's system-info doc](../../../../base/clean-core/docs/systems/system-info.md) for the cc half.
