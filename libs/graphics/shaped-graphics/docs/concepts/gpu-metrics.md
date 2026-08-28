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

## Load is a rate, so it needs a sampler

Busy time is a monotone counter, exactly like CPU time.
A `query_gpu_load()` returning a percentage could only work by keeping a hidden previous reading, which is the thing
`cc::cpu_load_sampler` exists to avoid — so the shapes match:

```cpp
ctx.read_gpu_counters();                           // result<gpu_counters> — monotone busy_secs per engine class
auto sampler = sg::gpu_load_sampler(ctx);          // holds its own baseline; borrows the context
auto const load = sampler.sample();                // result<gpu_load>
load.value().total;                                // f32 in [0,1] — the BUSIEST engine
load.value().per_engine;                           // which engine that was
load.value().interval_secs;                        // what the reading covers
```

**A GPU is several engines running at once** — 3D, copy, video decode, video encode — so `total` is the maximum across
them, never the sum and never the mean.
The sum could exceed 1 on a four-engine device; the mean hides a saturated copy engine behind three idle ones.
The maximum is what a task manager shows as "GPU %", and it is the number that answers "is the GPU the bottleneck".

Engines are matched **by name** between two readings, because the reported set can change and pairing by index would
difference two unrelated counters.

### Windows

The `GPU Engine` performance counters, read through PDH — the same source a task manager uses.

**Raw values rather than formatted ones.**
`Running Time` is a cumulative 100 ns counter, and PDH will format it into a rate against *its own* previous sample,
which would put back the hidden baseline the sampler exists to remove.

The counters are per process per engine: an instance is named
`raw:pid_1234_luid_0x00000000_0x0000C4B7_phys_0_eng_1_engtype_3D`, so a device's 3D busy time is the sum over every
process using it.
Grouping by `engtype` turns dozens of instances into the handful of numbers a reader wants.

The PDH query is opened once and shared behind a mutex: opening one walks the performance registry and costs
milliseconds, and a dashboard reads this every frame.

### Elsewhere

`read_gpu_counters` refuses, and the sampler refuses with it.
The routes exist but are not implemented: `/sys/class/drm/card*/device/gpu_busy_percent` on Linux, which amdgpu
provides and i915 does not, and IOKit `PerformanceStatistics` on macOS.

**A refusal beats a zero**: a zero draws as an idle GPU on a machine that is pinned.

## Stamping a recording

sg registers an `sg.gpu` stamp contributor when a context is created, so a `cc::rec` recording says which adapter the
run used — name, ids, driver version, board memory.
That is how a crash report identifies the GPU while cc goes on knowing nothing about GPUs.

The section describes the **first** context's adapter: a stamp provider hands back a span, so the bytes must outlive
the call, and a program creating several contexts would otherwise rewrite a buffer a recorder is reading.

See [clean-core's system-info doc](../../../../base/clean-core/docs/systems/system-info.md) for the cc half.
