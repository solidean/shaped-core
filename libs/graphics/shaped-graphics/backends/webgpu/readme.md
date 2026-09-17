# sg WebGPU backend

`sg::backend::webgpu` implements `sg::context` over WebGPU, on WebAssembly through Emscripten's **emdawnwebgpu** port.
It covers the whole sg surface except ray tracing.

```cpp
auto ctx = co_await sg::request_webgpu_context();               // adapter + device, settled from WebGPU's promises
auto wrapped = sg::create_webgpu_context(device);               // or wrap a WGPUDevice someone else requested
```

It builds only for the Emscripten WebGPU presets (`SC_WASM_WEBGPU=ON`), and it includes nothing but `<webgpu/webgpu.h>`.
A native Dawn build would be an additive CMake gate over the same sources.

## It never blocks

A browser settles a promise only once the task holding the thread returns, so a loop waiting on a callback has taken the only thread that callback could run on.
So `ctx.execution()` is `never_block`, both `block_until_` spellings assert, and every internal wait is unreachable.

- **Completion** arrives through `queue.onSubmittedWorkDone`, registered once per submit and once per epoch advance.
  The callback raises the completed counters and settles the due completion asyncs, so `arm_completion_signal` has nothing to arm.
- **Threading** is `single_threaded`: everything, callbacks included, runs on the thread that owns the device.
- **Pipelines** build through `createComputePipelineAsync` / `createRenderPipelineAsync` for `ctx.cached`, and synchronously for `ctx.uncached`.
- **Tests** run under node (Dawn, through the pinned `webgpu` npm package) and deno (wgpu), with nexus's executor returning to the JS event loop between steps.

## What WebGPU lacks, and what sg does about it

| sg feature | WebGPU | here |
|---|---|---|
| inline constants | no push constants | group 3 binding 0: a uniform bound with a dynamic offset into a 64 KiB page leased per list |
| `bound_sampler` | no pipeline-level samplers | group 3 binding `index + 1`; two at one index are refused |
| static samplers | none | a sampler entry in the declaring group, whose object the group inserts |
| 1D textures | 1D allows no mips, arrays, storage or render use | every sg 1D texture is a 2D texture of height 1 |
| memory heaps | none | a heap that places nothing: a placed buffer gets its own allocation, silently |
| binding arrays, staging groups, bindless | none in core | refused; `ctx.supports(sg::feature::binding_arrays)` is false |
| ray tracing, geometry, tessellation | none | refused; the matching features are false |
| `clamp_border`, `mirror_clamp_edge` | no such address modes | approximated as clamp-to-edge and mirror-repeat |

Group 3 is sg's reserved group on every backend, which is what lets one pipeline layout fit them all.
Slots below the caller's groups are filled with empty layouts wherever group 3 exists, since WebGPU numbers groups contiguously.

[docs/wgsl.md](docs/wgsl.md) is what a shader author needs from this table.

## Transfers

WebGPU has one queue, maps asynchronously, and requires copies in whole 4-byte words with texture rows at multiples of 256 bytes.

- **`cmd.upload`** writes into an unmapped staging ring with `queue.writeBuffer` at record time and records a copy out of it.
  A write lands in queue order ahead of the submit carrying the copy, so the copy reads exactly what was staged.
  The ring rewinds whenever no open list holds a span, and an upload it cannot hold gets a buffer of its own with a warning.
- **`cmd.download`** copies into a pooled `MAP_READ` buffer and maps it after the submit, delivering from the map's callback.
- **`ctx.upload` / `ctx.download`** write straight to the queue, and read back through their own one-copy submit.
- **`ctx.stream`** paces uploads to a window of bytes per pump sweep, in priority order.
  Writes into one resource keep their order, so an async upload behind a stalled stream still lands last.
  A list touching a resource a stream is still filling brings the rest of that stream forward at submit, warning once unless it was promoted.
- **Buffers** are allocated to a whole number of words.
  A write that is not a whole number of words must end at the buffer's end, where the rest is padding; anything else asserts.

## Presentation

A headless swapchain rotates `buffer_count` render targets, as dx12 emulates it.
A `sg::window_platform::web_canvas` swapchain configures a WGPUSurface on the canvas its CSS selector names; the page composites it when the rendering task returns.

## Errors

WebGPU reports validation and out-of-memory errors from a later task than the call that caused them.
They reach `ctx.take_pending_errors()`, and `webgpu_context::set_message_callback` as they arrive, which is how the test drivers fail on them.

## Files

```text
webgpu_context.hh/.cc/.create.cc   the context, its bring-up, and shutdown
webgpu_epoch.cc                    epochs, submission completion, deferred release
webgpu_resources.cc                buffers, textures (1D as 2D), the stub heap
webgpu_transfer.cc                 the upload ring, the readback pool, the constant pages, the sampler cache
webgpu_stream.hh/.cc               the async and streaming tiers
webgpu_binding.cc                  group layouts, pipeline layouts with group 3, binding groups
webgpu_pipeline.hh/.cc             compute and raster pipelines, both build paths
webgpu_command_list.*              recording: transfers, compute, raster, and pass management
webgpu_query.hh/.cc                timestamp queries
webgpu_swapchain.hh/.cc            headless and web canvas presentation
tests/                             shaped-graphics-webgpu-test, over hand-written WGSL
```
