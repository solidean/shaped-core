# Concept: metrics and stats

`ctx.metrics` is what a context reports about the device it runs on and about its own activity.
The adapter, GPU memory and the engine busy counters are [gpu-metrics](gpu-metrics.md); this page is about the **stats**.

## A stat is a monotone total

Each `sg::stat` is an `i64` that only ever grows: draws, dispatches, submitted lists, barriers, bytes moved per transfer path, resources created, time spent blocked on the GPU.
There is no reset and no "per frame" built in.
A caller who wants one takes two readings and subtracts:

```cpp
auto const before = ctx.metrics.stats();
// ... record, submit, advance the epoch ...
auto const frame = ctx.metrics.stats() - before;
auto const barriers = frame[sg::stat::buffer_barriers] + frame[sg::stat::texture_barriers];
```

All stats are integers, times included: `gpu_wait_nanoseconds` is a count of nanoseconds, whose unit `cc::rec::unit_nanoseconds` renders as seconds.
`sg::info_of(stat)` gives each one's recorded name, unit and a one-line description of what it counts, which is what a HUD iterates to show them all.

## When a reading includes what

**A command list's counts join the totals when it is submitted.**
Draws, dispatches, inline transfer bytes and the barriers recorded between ops are counted on the list while it records, on its one thread, and folded in under the submission lock.
A dropped list counts nothing, because it did nothing on the GPU.
The barriers a list needs on entry are resolved at its submit and counted there too.

**Context-level events are counted at the call**: an async or stream transfer's bytes when it is enqueued, a created resource when it is asked for, an epoch when it advances.
Counting a transfer at its enqueue rather than its completion keeps a reading deterministic: a test sees its own upload without waiting on a copy thread.
A stream from a buffer source is the exception: its size is unknown until its chunks exist, so it counts each chunk as the copy thread takes it.

So a reading is current as of the calls that caused it, on the thread that made them.
One taken while another thread submits may include part of that list, since each total is its own relaxed atomic.
A reading taken right after `advance_epoch`, on the thread that advanced, is a complete epoch.

## A stat a backend cannot count

**An uncounted stat reads zero, and `is_counted` says so.**
A barrier count on webgpu is zero because the WebGPU implementation tracks usage and nothing sg can see is emitted, not because there were none.
Metal's barriers name stages and never a resource, so it counts `global_barriers` but no buffer or texture barriers.
A test asserting "no barriers" checks `is_counted` first, or it passes on a zero nobody measured.

## What counts as a barrier

**Barriers are counted as the backend emits them.**
One per buffer or texture barrier record, one `barrier_calls` per call that submits a batch, and a texture barrier that changes the layout is also a `texture_transitions`.
A barrier naming stages and no resource is a `global_barriers`.
That is the cost a driver and a GPU profiler see, which is what the count is for; the same usage can count differently on two backends where a texture's subresources split differently.
Only the barriers on the command-list queue are counted, not the transfer queues'.

**A render-pass split is not a barrier and is counted separately.**
It is any rendering scope ended and reopened mid-scope, whatever forced it.
A fragment shader writing what the next draw reads costs vulkan an ended and reopened rendering, metal a reopened encoder and webgpu a reopened pass.
A copy recorded inside the scope costs webgpu one too.
Each is more expensive than a barrier, and invisible in a barrier count.

## Time blocked on the GPU

**`gpu_wait_nanoseconds` is the time spent inside the context's own waits on the GPU, and which waits those are varies per backend.**
Compare it across frames on one backend, not across backends.
vulkan counts the wait its inline-download actor makes for every readback, although no caller thread blocked; dx12 does not count that wait.
metal pumps other work while it waits, and that work's time counts too.
No backend counts a swapchain's frame-pacing waits.

## Stats in a recording

`advance_epoch` records each counted stat's change over the epoch as one `CC_RECORD_ACCUM` under its `sg.` name.
So a trace shows per-epoch counts beside the frame's scopes without anyone reading `ctx.metrics`.
