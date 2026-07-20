# shaped-graphics cheat sheet

Graphics-API wrapper. Namespace `sg`. Depends on clean-core + typed-geometry. Headers are
included by full path from `src/`: `#include <shaped-graphics/<name>.hh>`.

> **Scope note:** this sheet covers the small surface that exists today. The **dx12** backend is
> real (device / command list / GPU buffer); the sg core abstract API and the **vulkan** backend are
> still stubs.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

> **Error handling** (see [docs/error-handling.md](../../../docs/error-handling.md)): resource creates
> come in two flavors — a throwing default `create_*` (returns the handle, raises a typed
> `sg::exception` on failure) and a fallible `try_create_*` (returns `cc::result`, for exception-free
> callers / local fallback). `create_command_list()` is infallible (returns the handle; throws only on
> device loss). Contract violations (bad size, missing usage, null args, using a transient resource past
> its epoch) `CC_ASSERT` — they are bugs, not runtime failures. Device loss is sticky: `is_device_lost()`
> / `device_loss_reason()`, and submit / advance / fence waits throw `sg::device_lost_exception`.

How to read this: each block leads with the include; one symbol per line with a trailing
comment giving the return type / intuition.

---

## Handles & types

```cpp
#include <shaped-graphics/fwd.hh>
sg::context_handle        // std::shared_ptr<sg::context>        — shared, long-lived driver
sg::raw_buffer_handle     // std::shared_ptr<sg::raw_buffer const>   — shared-immutable resource
sg::raw_texture_handle    // std::shared_ptr<sg::raw_texture const>  — shared-immutable resource
sg::swapchain_handle      // std::shared_ptr<sg::swapchain>          — MUTABLE per-frame present driver (acquire/present)
// command_list has NO handle: it's a move-only temporary — std::unique_ptr<sg::command_list>,
// passed around by reference (command_list&). record once, submit once, not reused.
sg::async_compiled_shader   // std::shared_ptr<cc::async<compiled_shader>>   — async compile result (try_value() -> compiled_shader_handle)
sg::async_compute_pipeline  // std::shared_ptr<cc::async<compute_pipeline_handle>> — async PSO build (blocking_get -> compute_pipeline_handle)
// layout acquires (group + pipeline) are SYNC (cheap) — no async_* typedef for them.
// cc::async<T> can't hold a const T, so const lands at the read side (try_value yields the const *_handle).
```

## bytes_future / bytes_waiter — download results

```cpp
#include <shaped-graphics/bytes_future.hh>
sg::bytes_future                    // returned by cmd.download.bytes_from_buffer; holds {span, pin, waiter}
f.is_valid()                        // bool — backed by a real download (vs default-constructed)
f.is_ready()                        // bool — NON-BLOCKING poll; true once the actor copied the bytes back
f.try_get_bytes()                   // -> cc::optional<cc::pinned_data<cc::byte const>>  (polls; nullopt until ready)
sg::data_future<T>                  // typed wrapper: try_get_data() -> cc::optional<cc::pinned_data<T const>>
sg::bytes_waiter                    // abstract poll handle a backend subclasses; sg::ready_bytes_waiter = ready-on-construction
// to BLOCK until a download is delivered, use ctx.wait_for(future) (see epochs) — the future has no blocking wait
```

## Enums

```cpp
#include <shaped-graphics/types.hh>
sg::backend_kind          // dx12, vulkan, metal, webgpu, opengl, webgl
sg::thread_model          // single_threaded | multi_threaded (see docs/concepts/threading.md)
sg::buffer_usage          // bit flags named by operation: none/copy_src/copy_dst/vertex_buffer/index_buffer/
                          //   uniform_buffer/readonly_buffer/readwrite_buffer/indirect_command_buffer/
                          //   accel_structure_{storage,build_input}
                          //   (granularity set by Vulkan; DX12 typeless, Metal untyped — they consume a subset)
a | b                     // combine usages
sg::has_flag(usage, flag) // bool — every bit of `flag` set in `usage`
sg::present_mode          // vsync | immediate  (swapchain frame pacing — see the swapchain section)
```

## context — mutable driver / factory

```cpp
#include <shaped-graphics/context.hh>
ctx.backend()                                      // sg::backend_kind (coarse tag, not identity)
ctx.accepted_shader_formats()                      // span<shader_format const>, most-preferred first, never empty (dx12 -> dxil, vulkan -> spirv)
ctx.accepts_shader_format(f)                       // bool — hand this to slib's acquire(ctx) rather than assuming a format; see docs/shaders.md
ctx.threading()                                    // sg::thread_model — which ops are concurrency-safe
ctx.is_device_lost() / ctx.device_loss_reason()    // bool / string_view — sticky device-lost status (see Error handling above)
ctx.create_command_list()                          // -> std::unique_ptr<command_list> (already recording); infallible (throws only on device loss)
ctx.create_swapchain(swapchain_description = {})   // -> swapchain_handle (throws sg::swapchain_creation_exception / device_lost); see the swapchain section
ctx.try_create_swapchain(swapchain_description = {})  // -> cc::result<swapchain_handle>  (fallible twin)
// PREFER the typed factories below (create_buffer<T> / create_texture_2d) — raw_* is the byte-level escape hatch.
// PREFER ctx.transient for anything sized by the current frame; ctx.persistent only for what outlives it.
ctx.persistent.create_raw_buffer(size, usage, alloc={})     // -> raw_buffer_handle  (throws sg::allocation_exception; size>=0, 0 = empty, no alloc)
ctx.persistent.try_create_raw_buffer(size, usage, alloc={}) // -> cc::result<raw_buffer_handle>  (fallible core; every create_* has a try_ twin)
                                                   //   resource creation lives on the lifetime scope (sg::context_persistent_scope)
                                                   //   alloc defaults to dedicated; pass a placed allocation_info (from a heap) to sub-allocate
ctx.persistent.create_memory_heap(size)            // -> memory_heap_handle  (heap placed resources sub-allocate into; try_create_memory_heap for the result form)
ctx.transient.create_raw_buffer(size, usage)       // -> raw_buffer_handle  per-epoch scratch (bump-reset heap); expires at advance_epoch (+ try_ twin)
ctx.transient.set_budget(size)                     // void — shared transient heap budget (buffers + future textures); applied at the next advance_epoch; default 128 MiB
ctx.transient.create_binding_group(layout, views)  // -> binding_group_handle  transient (ring-allocated) group; expires with its epoch (+ try_ twin)
ctx.upload.bytes_to_buffer(buf, cc::pinned_data<byte const>, offset_in_bytes=0)  // void — ASYNC stream host bytes into buf on the copy queue (needs copy_dst); fire-and-forget, pin holds the bytes; later lists reading buf auto-wait; empty = no-op
ctx.upload.data_to_buffer(buf, cc::pinned_data<T const>, offset_in_elements=0)   // void — typed convenience; re-views the SAME pin as bytes (no copy). offset in ELEMENTS of T. build the pin with cc::make_pinned_data / cc::as_pinned_data
                                                   //   buf may be a raw_buffer_handle OR a buffer<T> — pass the typed buffer and T is deduced (no .raw())
ctx.upload.bytes_to_texture(tex, cc::pinned_data<byte const>, subresource={}, region={})  // void — ASYNC upload tightly-packed pixels into one texture (sub)region (needs copy_dst); later lists reading tex auto-wait
ctx.upload.set_async_window_size(bytes)            // void — resize the async staging window (x3 buffered); copy actor adopts it between windows; default 16 MiB
ctx.upload.set_inline_budget(bytes)                // void — resize the inline (cmd.upload) ring; applied at the next advance_epoch; default 16 MiB
ctx.download.bytes_from_buffer(buf, offset_in_bytes, size)    // -> sg::bytes_future — ASYNC read buf back on the copy queue (needs copy_src); read auto-waits on the last writer, a later writer auto-waits on the read; drop the future to cancel; size 0 = ready empty future
ctx.download.data_from_buffer<T>(buf, off_in_elements, count) // -> sg::data_future<T>; offset AND count in ELEMENTS of T. See bytes_from_buffer
ctx.download.data_from_buffer(typed_buf[, off, count])        // -> sg::data_future<T> — T deduced from buffer<T>; no args past the buffer = whole buffer
ctx.download.bytes_from_texture(tex, subresource={}, region={}) // -> sg::bytes_future — ASYNC read one texture (sub)region back (needs copy_src), tightly packed
ctx.download.set_async_window_size(bytes)          // void — resize the async readback staging window (x3 buffered); copy actor adopts it between windows; default 16 MiB
ctx.download.set_budget(bytes)                      // void — resize the inline (cmd.download) readback ring; applied at the next advance_epoch (drains the readback actor); default 16 MiB
                                                   //   using any transient resource past its epoch is a hard error (asserts)
ctx.submit_command_list(std::move(cmd))            // -> submission_token — consumes cmd (submit once; same epoch it opened in); throws sg::device_lost_exception on device loss
ctx.submit_command_list_and_present(sc, std::move(cmd)) // -> submission_token — THE present path: folds the swapchain back-buffer's present-layout transition into cmd, submits, then presents (see swapchain)
ctx.drop_command_list(std::move(cmd))              // void — consumes cmd; explicit discard (same epoch). NB a list left to leave
                                                   //   scope un-consumed auto-drops itself but PRINTS A WARNING — submit or drop it explicitly
ctx.shutdown()                                     // void — release backend state; virtual; idempotent; auto-run by backend dtor
ctx.is_shut_down()                                 // bool
// invariant: a context must OUTLIVE every command list & buffer it created (must be shut down before dtor)
// you never call sg::create_context — there is none. each backend library provides the factory:
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
sg::create_vulkan_context(vulkan_config = {})      // -> cc::result<context_handle>
// vulkan_config { bool enable_validation_layers=false; bool prefer_software_device=false; }  (independent flags)
#include <shaped-graphics/backends/dx12/dx12_context.hh>
sg::create_dx12_context(dx12_config = {})          // -> cc::result<context_handle>
// dx12_config { bool enable_debug_layer=false; bool use_warp=false; }  (independent flags)
// create errors on environment failure (no adapter, device refused); misuse asserts
```

## exceptions — thrown by the create_* façades + submit/advance  (see docs/error-handling.md)

```cpp
#include <shaped-graphics/exceptions.hh>
sg::exception                    // base; .message() -> cc::string_view. catch this for "any sg failure"
sg::device_lost_exception        // device lost (sticky); .reason(). from submit/advance/fence waits + throwing creates
sg::allocation_exception         // resource/heap OOM or exhaustion; .size_in_bytes()
sg::pipeline_creation_exception  // binding_group_layout / pipeline_layout / compute|raster|raytracing pipeline build failure; .entry_point()
sg::binding_group_exception      // binding_group wiring error (unknown/missing binding, kind mismatch) or descriptor exhaustion
sg::swapchain_creation_exception // create_swapchain failure (bad window / format / DXGI error)
// only the throwing create_* and submit/advance raise these; the try_create_* surface never throws
```

## epochs — frame-level GPU lifetime + CPU↔GPU sync  (see docs/concepts/epochs.md)

```cpp
#include <shaped-graphics/fwd.hh>
sg::epoch                 // enum class : u64 — invalid=0, first=10000; monotonic frame token + fence value
sg::submission_token      // enum class : u64 — invalid=0, first=30000, not_submitted=~0; per-command-list token
ctx.current_epoch()                     // sg::epoch — epoch new work records into
ctx.completed_epoch()                   // sg::epoch — latest fully-finished epoch (reclaimable)
ctx.advance_epoch(allowed_in_flight)    // void — close current epoch, open next; cc::optional<int>:
                                        //   nullopt=never wait, 0=full drain, N=keep <=N epochs in flight
ctx.advance_epoch_and_wait_for_idle()   // void — spelled-out advance_epoch(0); advance never hidden
ctx.process_completed_epochs()          // void — retire finished epochs (free resources, run finalizers)
ctx.wait_for_epoch(e)                   // void — block until epoch e done, then retire (does NOT advance)
ctx.wait_for_next_inflight_epoch()      // void — block on oldest in-flight epoch (back-pressure; no advance)
ctx.wait_for(future)                    // -> cc::optional<cc::pinned_data<...>> (bytes/typed) — BLOCK until a download is
                                        //   delivered, then return it; nullopt if invalid/unsubmitted/cancelled. The ONLY
                                        //   guaranteed-complete call: advance_epoch* / wait_for_idle drain the GPU but NOT
                                        //   the readback actor, so is_ready() can lag them. Waitable once its list is
                                        //   submitted (no advance needed); touches no ctx state, safe from any thread.
ctx.is_submission_complete(token)       // bool — has that one command list finished?
// command lists cannot span epochs (submit/drop in the epoch opened in — CC_ASSERT-enforced)
// on multi_threaded backends: create/submit/drop, the wait_*/process_completed_epochs retire family, and
//   wait_for(future) are all concurrency-safe (any thread); only advance_*/shutdown must be externally synchronized
cmd.created_in_epoch()                  // sg::epoch — the epoch this command list was opened in
cmd.context()                           // sg::context& — the context that created the list (outlives it); reach it without threading ctx separately
buf->add_finalizer([]{ ... })           // void — runs after the GPU handle is freed AND no longer in flight
```

## command_list — records GPU work  (abstract)

```cpp
#include <shaped-graphics/command_list.hh>
// abstract; a backend subclasses it (protected ctor). obtained via ctx.create_command_list()
// -> std::unique_ptr; passed by reference (command_list&). record once; submit OR drop it once,
// explicitly, in the epoch it opened in (not reused). Leaving it to go out of scope auto-drops it + warns.
cmd.upload.bytes_to_buffer(buf, bytes, offset_in_bytes=0)     // void — stage host bytes into buf (needs copy_dst); empty = no-op
cmd.upload.data_to_buffer(buf, range, offset_in_elements=0)   // void — typed convenience (trivially-copyable contiguous range); offset in ELEMENTS
cmd.upload.pod_to_buffer(buf, value, offset_in_elements=0)    // void — single trivially-copyable value; offset in ELEMENTS of T (bytes_to_buffer is the fractional-offset escape hatch)
                                                              //   PREFER passing a buffer<T> as `buf`: the data param is then span<T const> and value is T — one T, no static_assert needed. .raw() in a transfer call = a missing overload
                                                              //   (typed form takes span<T const>, so vector / C array / {1,2,3} all convert; only the raw form accepts any contiguous_range)
cmd.upload.bytes_to_texture(tex, bytes, subresource={}, region={})  // void — inline upload tightly-packed pixels into one texture (sub)region (needs copy_dst); drives the copy_dst layout barrier; visible to later cmds in the list
cmd.download.bytes_from_buffer(buf, offset_in_bytes, size)    // -> sg::bytes_future (needs copy_src); size 0 = ready empty future
cmd.download.data_from_buffer<T>(buf, off_in_elements, count) // -> sg::data_future<T>; offset AND count in ELEMENTS of T
cmd.download.data_from_buffer(typed_buf[, off, count])        // -> sg::data_future<T> — T deduced from buffer<T>; no args past the buffer = whole buffer
cmd.download.bytes_from_texture(tex, subresource={}, region={}) // -> sg::bytes_future — inline read one texture (sub)region back (needs copy_src), tightly packed; ready once the submitted list runs
sg::subresource_index  // { int mip_level=0; int array_layer=0; texture_aspect aspect=color }  — addresses one subresource (point analog of subresource_range); <shaped-graphics/backend/subresource.hh>
sg::texture_region     // { tg::pos3i offset; tg::vec3i size } — a texel box. the copy APIs take cc::optional<texture_region>: none = whole subresource, empty (size<=0) = no-op, else bounds-checked. block-aligned for BC. host bytes TIGHTLY packed (row = width-in-blocks × block-bytes); <shaped-graphics/texture_region.hh>
cmd.copy.buffer_bytes_region({.src, .dst, .size_in_bytes, .src_offset_in_bytes=0, .dst_offset_in_bytes=0}) // void — device→device buffer copy (src needs copy_src, dst needs copy_dst); size 0 = no-op
cmd.copy.buffer_data_region<T>({.src, .dst, .count, .src_offset=0, .dst_offset=0}) // void — typed convenience (count + offsets in elements of T; like a subspan)
// cmd.upload/download = INLINE (recorded in this list); ctx.upload/download = ASYNC (copy queue, off the
// frame path — for bulk streaming/readback). See docs/concepts/{upload,download}.async.md.
// inline path: copy is recorded here; the download future is delivered by a separate actor after the
// submitted list finishes on the GPU (no advance_epoch needed — but advance_epoch* / wait_for_idle do NOT
// guarantee delivery either; use ctx.wait_for(future)). Uploading + downloading + copying the SAME buffer works in ONE list —
// the access tracker orders them (see docs/concepts/barriers.md). Self-copy needs non-overlapping ranges.
// vulkan transfer is a TODO stub.

// GPU queries (cmd.query scope). See docs/concepts/queries.md.
cmd.query.is_supported()               // bool — backend/device supports GPU timestamps? (dx12 direct queue: yes; vulkan stub: no)
cmd.query.record_gpu_timestamp()       // -> sg::gpu_timestamp — record a point-in-time GPU tick here; invalid if unsupported
// resolved + read back at submit (one batched readback per 4096-slot query heap; more records lease more heaps).

// raster rendering scope (cmd.raster scope). Bind color / depth-stencil targets + apply per-target begin-ops,
// then bind a raster_pipeline and draw. vulkan is a stub. dx12 real on WARP.
auto pass = cmd.raster.render_to({.color_targets={rtv.cleared(tg::vec4f(1,0,0,1))},       // -> sg::rendering_scope (RAII)
                                  .depth_stencil_target=dsv.cleared(1.0f)});              //   end_rendering() at scope exit
// view builders: view.cleared(color/depth[,stencil]) | view.preserved() | view.discarded() -> color_target / depth_stencil_target
// rendering_info { fixed_vector<color_target,max_color_targets> color_targets; optional<depth_stencil_target>; optional<viewport>; optional<tg::aabb2i> scissor }
//   viewport/scissor unset => full target extent. sg::viewport { tg::pos2f offset; tg::vec2f size; float min_depth=0, max_depth=1 }
cmd.raster.manual.begin_rendering(info) / .end_rendering()   // void — same, by hand (must balance); prefer render_to

// draw recording — on cmd.raster / cmd.raster.manual (NOT the rendering_scope handle); valid while a scope is open:
cmd.raster.bind_pipeline(raster_pipeline)               // void — active raster PSO + IA topology + graphics root sig
cmd.raster.bind_group(set, binding_group)              // void — bind at slot `set` (indexes the pipeline layout's groups)
cmd.raster.bind_vertex_buffers({vbuf->as_vertex_buffer<Vtx>()}, first_slot=0)  // void — also: bind_vertex_buffer(view, slot) / span overload
cmd.raster.bind_index_buffer(ibuf->as_index_buffer(sg::index_format::uint16))  // void
cmd.raster.set_viewport(vp) / .set_scissor(rect)       // void — override the scope's viewport / scissor
cmd.raster.set_stencil_reference(u32) / .set_blend_constants(tg::vec4f)  // void — dynamic depth-stencil / blend state
cmd.raster.set_inline_constants(data|POD, offset={})   // void — root/push constants (same as cmd.compute)
cmd.raster.draw({.vertex_range={.offset=0,.size=3}, .instance_range={.offset=0,.size=1}})   // void — ranges are cc::offset_size {first, count}
cmd.raster.draw_indexed({.index_range={.offset=0,.size=N}, .instance_range={.offset=0,.size=1}, .vertex_offset=0})  // void
```

## sg::gpu_timestamp — result of cmd.query.record_gpu_timestamp

```cpp
#include <shaped-graphics/gpu_timestamp.hh>
sg::gpu_timestamp t = cmd.query.record_gpu_timestamp(); // copyable value type; read AFTER submitting the list
t.is_valid()                    // bool — backed by a real query (false = default-constructed / unsupported backend)
t.is_ready()                    // bool — NON-BLOCKING poll; true once the tick landed (false before submit / forever if dropped)
t.try_get_ticks()               // -> cc::optional<cc::u64>  — raw GPU tick (polls); only DIFFERENCES are meaningful
t.try_get_seconds()             // -> cc::optional<double>   — tick * (1/frequency) (polls)
ctx.wait_for_ticks(t)           // -> cc::optional<cc::u64>  — BLOCK until delivered, returns the tick
ctx.wait_for_seconds(t)         // -> cc::optional<double>   — same, returns seconds
// normal per-frame usage: poll is_ready() a frame or two later, don't block. Two timestamps around work = its GPU duration.
```

## raw_buffer — GPU-resident, immutable shape  (abstract)

```cpp
#include <shaped-graphics/raw_buffer.hh>
// abstract; a backend subclasses it. protected ctor: raw_buffer(size_in_bytes, usage)  (size 0 = empty)
b.size_in_bytes()                  // isize   (inline, cheap — no virtual call)
b.usage()                          // sg::buffer_usage
b.is_expired() / b.is_valid()      // bool    — storage reclaimed? transient auto-expires at advance_epoch
b.expire()                         // void    — free storage now (deferred); explicit early-free for persistent
// shape metadata (_size_in_bytes/_usage) is protected in the base; backend buffers inherit it
// view factories are BYTE-LEVEL only (no C++ element type); the buffer's usage must cover the access.
// For the ergonomic, element-typed views (as_readonly_buffer(), as_uniform_buffer(), …) wrap in buffer<T>.
b.as_raw_readonly({.offset=,.size=})       // -> raw_buffer_view (byte-addressed SRV, shape=raw; range in bytes; default = whole)
b.as_raw_readonly({.offset=,.size=}, stride)// -> raw_buffer_view (STRUCTURED SRV; explicit byte stride; element_count = size/stride)
b.as_raw_readwrite({.offset=,.size=})      // -> raw_buffer_view (byte-addressed UAV, shape=raw)
b.as_raw_readwrite({.offset=,.size=}, stride)// -> raw_buffer_view (STRUCTURED UAV; explicit byte stride)
// EVERY storage view (raw or structured) is a SUBRANGE, so: offset % 256 == 0 (WebGPU minStorageBufferOffset-
//   Alignment; some Vulkan hw) and size % 4 == 0 (WebGPU). Structured ALSO needs offset % stride == 0 and
//   size % stride == 0 (D3D12 addresses by element index: FirstElement = offset/stride).
// buffer<T> itself is exempt — it's a whole buffer recast like a span (so buffer<u16> index buffers are fine),
//   as are the draw-input views (as_vertex_buffer / as_index_buffer).
// Bypass: build the raw_buffer_view aggregate yourself. For a heterogeneous buffer: one WHOLE-buffer raw view
//   + in-shader Load<T>(byteOffset) does per-object addressing — see docs/concepts/views.md.
// try_ TWINS: every storage/uniform factory above has one (try_as_raw_readonly/readwrite/uniform_buffer, both
//   overloads + whole-buffer) -> cc::optional, nullopt when the RANGE is bad (bounds / 256 / %4 / stride).
//   A missing buffer_usage flag still ASSERTS (you chose usage at creation). Draw-input views have no twin.
b.as_raw_uniform_buffer({.offset=,.size=}) // -> sg::raw_buffer_view (uniform_block; offset 256-aligned; size <= 64 KiB)
b.as_raw_vertex_buffer({.offset=,.size=}, stride_in_bytes)  // -> vertex_buffer_view (explicit stride)
b.as_index_buffer(format)                  // -> index_buffer_view (whole buffer)
b.as_raw_index_buffer(format, {.offset=,.size=})            // -> index_buffer_view (byte range; width from format)
// re-type up to the typed wrapper (inverse of buffer<T>::raw()); same shape check as buffer<T>::from_raw:
b.as_buffer<T>()                           // -> buffer<T>              (asserts byte size is a whole number of T)
b.try_as_buffer<T>()                       // -> cc::optional<buffer<T>> (nullopt on a trailing partial element)
```

## buffer<T> — typed buffer wrapper  (element type fixed at compile time)

```cpp
#include <shaped-graphics/buffer.hh>       // the typed buffer<T> wrapper
sg::buffer<T>                              // GPU-side span<T>: wraps a raw_buffer_handle, T fixed at compile time
// create typed (preferred): element_count -> byte size = count * sizeof(T); returns the wrapped buffer<T>:
ctx.persistent.create_buffer<Particle>(1000, usage, alloc={})  // -> sg::buffer<Particle>  (+ try_ twin)
ctx.transient.create_buffer<Particle>(64, usage)               // -> sg::buffer<Particle>  (transient; no allocation_info)
sg::buffer<T>::from_raw(raw_handle)         // wrap a raw handle: byte size must be a whole number of T (asserts); try_from_raw -> cc::optional
sg::buffer<T>::from_raw_clamped(raw_handle) // wrap, flooring to whole elements (a trailing partial element is ignored)
buf.reinterpret_as<U>()                     // -> buffer<U>; static_assert sizeof(T)%sizeof(U)==0 (U tiles T, e.g. buffer<vec3f>->buffer<float>)
buf.try_reinterpret_as<U>()                 // -> cc::optional<buffer<U>>; general case (any U); nullopt when size % sizeof(U) != 0
buf.element_count()                        // isize — size_in_bytes / sizeof(T) (truncates)
buf.size_in_bytes() / buf.usage()          // isize / sg::buffer_usage
// view factories infer the element type from T (no <T> spelled), else identical to raw_buffer's:
buf.as_uniform_buffer(element_index=0)     // -> uniform_buffer_view<T>    (binds ONE element as a cbuffer; byte offset element_index*sizeof(T) must be 256-aligned; only where T is a uniform_element)
buf.as_readonly_buffer({.offset=,.size=})  // -> readonly_buffer_view<T>   (only where T is a view_element; range in elements of T)
buf.as_readwrite_buffer({.offset=,.size=}) // -> readwrite_buffer_view<T>  (only where T is a view_element)
//   these are SUBRANGES: byte offset (= range.offset * sizeof(T)) must be 256-aligned, byte size a multiple of 4
buf.try_as_readonly_buffer(...) / try_as_readwrite_buffer(...) / try_as_uniform_buffer(idx)  // -> cc::optional<view>
//   nullopt on a bad range (bounds / 256 / %4); missing usage still asserts. Whole-buffer overloads have twins too.
buf.as_vertex_buffer() / (range)           // -> vertex_buffer_view (stride = sizeof(T); range in vertices of T)
                                           //   stride only for now — not yet tied to the pipeline's vertex_input_layout
buf.as_index_buffer() / (range)            // -> index_buffer_view  (only buffer<u16>/buffer<u32>; width follows T; range in indices)
```

## pixel_format — texel formats  (restrictive; all backends have an equivalent)

```cpp
#include <shaped-graphics/pixel_format.hh>
sg::pixel_format             // enum: undefined, r8/rg8/rgba8/bgra8 (unorm/snorm/uint/sint/srgb),
                             //   r/rg/rgba 16f & 32f & int, rgb10a2_unorm, rg11b10_float,
                             //   depth16_unorm/depth32_float/depth32_float_stencil8, bc1..bc7 (feature-gated)
sg::is_depth_format(f)       // bool  — depth or depth-stencil
sg::has_stencil(f)           // bool  — carries a stencil plane
sg::is_compressed_format(f)  // bool  — BC block-compressed (4x4 blocks)
sg::format_block_size(f)     // int   — bytes per texel, or per 4x4 block for BC (0 for undefined)
sg::format_block_extent(f)   // int   — 1 (uncompressed) or 4 (BC)
```

## texture — GPU-resident texture  (raw resource + typed wrapper)

```cpp
#include <shaped-graphics/raw_texture.hh>   // the raw resource + its description
#include <shaped-graphics/texture.hh>       // the typed texture<Traits> wrapper + shape typedefs
sg::texture_description      // { format, dimension(d1/d2/d3), width/height/depth, mip_levels,
                             //   array_layers (cc::optional<int>; nullopt = not an array),
                             //   sample_count (>1 = MSAA), is_cube, usage }
sg::raw_texture_handle       // std::shared_ptr<sg::raw_texture const> — the general/raw resource
t->width()/height()/depth()  // int — extents (height/depth per dimension)
t->mip_levels()/sample_count()/array_layers()  // int
t->format()                  // sg::pixel_format
t->is_array()/is_cube()/is_multisampled()      // bool  — derived shape queries
sg::texture_usage            // flags: copy_src/copy_dst, readonly_texture, readwrite_texture, render_target, depth_stencil
// create the raw resource (full desc; untyped handle):
ctx.persistent.create_raw_texture(desc)        // -> raw_texture_handle  (dedicated; throws sg::allocation_exception; + try_ twin)
ctx.transient.create_raw_texture(desc)         // -> raw_texture_handle  (dedicated for now; auto-expires; + try_ twin)
// typed factories (preferred): shape-specific description (only the free params) -> the wrapped texture<Traits>:
#include <shaped-graphics/texture_descriptions.hh>
ctx.persistent.create_texture_2d({.format=..., .width=256, .height=128, .usage=...})  // -> sg::texture_2d  (+ try_ twin)
ctx.transient.create_texture_2d({...})         // -> sg::texture_2d  (transient; no allocation_info param)
//   one per typedef: create_texture_1d/2d/3d/cube/1d_array/2d_array/cube_array/2d_ms/2d_array_ms/cube_ms/cube_array_ms
//   cubes take .size (edge length; width==height); cube arrays take .cube_count; MS take .sample_count (no .mip_levels)
//   generic core: create_texture(desc) / try_create_texture(desc) deduce the shape from the description type
// typed wrapper: shape fixed at compile time; getters gated by concepts (depth() only on 3D, etc.)
sg::texture_2d::from_raw(raw_handle)           // wrap a raw handle; asserts the raw shape matches (try_from_raw -> optional); .raw() -> raw_texture_handle
raw->as_texture_2d() / raw->try_as_texture_2d()// same, straight off the handle (one accessor per typedef: as_texture_1d/2d/3d/cube/…/cube_array_ms; try_ -> optional)
// Each factory takes a shape-specific param bag (Traits::*_params); ranges are view_range{start,count<0=all}.
// sampled (SRV) — needs readonly_texture usage. Natural dimension:
tex.as_readonly_view({.mips={.start=1}})       // -> readonly_texture_view<VT>  (VT deduced; whole; params name only this shape's axes)
//   read_only_params fields: .mips always; .slices (arrays); .cubes (cube arrays)
tex.as_readonly_2d_view({.slice=3})            // array/cube -> Texture2D: one slice/.face/{.cube,.face}
tex.as_readonly_1d_view({.slice=3})            // 1D array -> Texture1D
tex.as_readonly_cube_view({.cube=2})           // cube array -> one TextureCube
tex.as_readonly_2d_array_view({.slices={...}}) // cube / cube array -> Texture2DArray (faces as a flat 2D array)
// storage (UAV) — needs readwrite_texture; single mip; not on MS (a cube UAV is a 2D array):
tex.as_readwrite_view({.mip=1})                // -> readwrite_texture_view<VT>  (VT deduced; whole, natural dimension)
//   read_write_params fields: .mip always; .slices (arrays/cubes); .depth_slices (3D, the W/Z axis)
tex.as_readwrite_2d_view({.slice=3,.mip=0})    // array/cube -> Texture2D    tex.as_readwrite_1d_view({.slice=3})
// render-target / depth-stencil views (2D-shaped only; single mip; MSAA allowed; NOT shader-facing — no raw_view):
tex.as_render_target_view({.mip=1})            // -> render_target_view  (needs render_target usage + color format)
tex.as_depth_stencil_view()                    // -> depth_stencil_view  (needs depth_stencil usage + depth format)
tex.as_render_target_2d_view({.slice=2})       // array/cube -> one layer/face as a 2D target (also _depth_stencil_)
// typedefs: texture_1d/2d/3d, texture_cube, texture_1d_array/2d_array/cube_array,
//           texture_2d_ms/2d_array_ms/cube_ms/cube_array_ms
// bind a texture view in a compute dispatch → it auto-transitions to shader_read (SRV) / storage (UAV).
// NOTE: SRV/UAV/RTV/DSV + samplers exist; texture upload/download/copy and the render-pass consumer remain future.
```

## views — strongly-typed resource views  (see docs/concepts/views.md)

```cpp
#include <shaped-graphics/views.hh>
sg::view_element<T>          // concept: T is `byte`, or sizeof(T) % 4 == 0 (GPUs load DWORD-aligned)
sg::uniform_element<T>       // concept: view_element + size multiple of 16 and <= 64 KiB (excludes byte)
sg::uniform_buffer_view<T>          // uniform block of T   (cbuffer/UBO)          — view_class::uniform
sg::readonly_buffer_view<T>         // read array of T      (SRV / read SSBO)      — view_class::readonly  (T=byte → raw)
sg::readwrite_buffer_view<T>        // rw array of T        (UAV / rw SSBO)        — view_class::readwrite (T=byte → raw)
// each holds a raw_buffer_handle + range; pure value (no GPU alloc). Made via buffer.as_*() above.
sg::readonly_texture_view<VT>  // sampled texture (SRV); VT = texture_view_traits<Dim> — view_class::readonly
sg::readwrite_texture_view<VT> // storage texture (UAV); VT constrained to storage_view_dimension (no cube/MS)
// each holds { raw_texture_handle, pixel_format, subresource_range }. Made via texture<Traits>.as_*_view() (returns the precise VT).
// view traits: tv_1d / tv_1d_array / tv_2d / tv_2d_array / tv_2d_ms / tv_2d_ms_array / tv_3d / tv_cube / tv_cube_array
sg::buffer_view<T>           // access-erased middle: any access of a buffer of T (access is a runtime field); leaves convert implicitly
sg::texture_view<VT>         // access-erased middle: any access of a texture view of dimension VT::dimension
sg::tlas_view                // ray-tracing TLAS (SRV, VA-addressed) — view_class::acceleration_structure. Via tlas.as_view()
sg::view_class               // uniform | readonly | readwrite | acceleration_structure   (access)
sg::view_shape               // uniform_block | structured | raw | texture | acceleration_structure   (layout)
sg::raw_view                 // = std::variant<raw_buffer_view, raw_texture_view, raw_tlas_view> — erased sum every typed view converts into
v.to_raw()  /  (implicit)    // -> raw_view; sg::access_of(rv) / sg::shape_of(rv) read the active arm's access/shape
// backends std::visit / get_if the arm (raw_buffer_view | raw_texture_view | raw_tlas_view) to build the native descriptor
// raw arms are also the directly-usable "raw" binding vocabulary for tooling
// INVERSE (erased -> typed leaf): as_* asserts (access, +dimension for textures); try_as_* -> cc::optional (nullopt on mismatch / wrong arm)
mid.as_readonly() / as_readwrite() / as_uniform()   // buffer_view<T> middle -> the leaf (only the runtime access is pinned)
mid.as_readonly() / as_readwrite()                  // texture_view<VT> middle -> the leaf (as_readwrite: storage VT only)
arm.as_readonly<T>() / as_readwrite<T>() / as_uniform<T>()   // raw_buffer_view arm -> leaf (you supply T)
arm.as_readonly<VT>() / as_readwrite<VT>()                   // raw_texture_view arm -> leaf (you supply VT; checks view dimension)
sg::as_readonly_buffer<T>(rv) / as_readwrite_buffer<T> / as_uniform_buffer<T>    // raw_view -> buffer leaf in one call (+ try_ twins)
sg::as_readonly_texture<VT>(rv) / as_readwrite_texture<VT>                       // raw_view -> texture leaf in one call (+ try_ twins)
// deferred: texel buffers (typed linear buffers). samplers: see sampler.hh
```

```cpp
// also in views.hh: render-target / depth-stencil target views — NOT shader-facing (do not erase to raw_view)
sg::render_target_view       // color render target (RTV) — made via texture<Traits>.as_render_target_view()
sg::depth_stencil_view       // depth-stencil target (DSV) — via .as_depth_stencil_view()
// each holds { raw_texture_handle, texture_view_dimension, pixel_format, subresource_range } (single mip).
v.texture() v.dimension() v.format() v.range()   // getters
v.width() v.height()         // int — the viewed mip's pixel size (mip-adjusted, >= 1)
v.size()                     // -> tg::vec2i (width, height) — drive a dispatch / transient texture with this
v.aspect_ratio()             // -> float  width/height; both clamped >= 1, so never divides by zero
// Do NOT erase to raw_view (never shader-visible / in a binding group); a backend consumes them directly.
sg::is_render_target_format(f) // bool — a renderable color format (not depth, not compressed, not undefined)
```

## swapchain — window presentation  (dx12 real; via ctx.create_swapchain)

```cpp
#include <shaped-graphics/swapchain.hh>
sg::swapchain_description       // { void* native_window_handle=nullptr (HWND on Windows); int buffer_count=2 (>=2);
                                //   pixel_format format=bgra8_unorm; present_mode present_mode=vsync; bool enable_hdr=false }
sg::present_mode                // vsync (wait for vblank) | immediate (uncapped, may tear)
auto sc = ctx.create_swapchain({.native_window_handle = hwnd});   // -> swapchain_handle (fallible twin: ctx.try_create_swapchain)
// per frame:
auto rt = sc->acquire_backbuffer();   // -> render_target_view for the current back buffer (auto-resizes to the window, once/epoch)
auto size = rt.size();                   // -> tg::vec2i, THIS frame's size — the swapchain has no size getter (a later acquire may resize)
float aspect = rt.aspect_ratio();        // -> float, for the projection; rt.width() / rt.height() are still there as ints
//   ... render into rt this frame (rt.cleared(color) / cmd.raster.render_to({.color_targets = {...}})) ...
ctx.submit_command_list_and_present(*sc, std::move(cmd));  // the present path: folds the present-layout transition into cmd,
                                                           //   submits, then presents — exactly one per successful acquire
sc->format() sc->buffer_count() sc->present_mode() sc->is_hdr_enabled() sc->native_window_handle() sc->description()
// acquire / submit_command_list_and_present throw sg::device_lost_exception on device loss. Bad handle / count / format asserts.
```

## samplers — how a shader reads a texture  (see docs/concepts/bindings.md)

```cpp
#include <shaped-graphics/sampler.hh>
sg::sampler                 // { min/mag/mip_filter; address_u/v/w; mip_lod_bias; max_anisotropy;
                            //   min/max_lod; cc::optional<compare_op> compare; sampler_border_color }  — value type, ==
                            //   defaults = trilinear, repeat, no anisotropy, no comparison (max_lod = sampler::lod_max)
sg::sampler_filter          // nearest | linear
sg::sampler_address_mode    // repeat | mirror_repeat | clamp_edge | clamp_border | mirror_clamp_edge
sg::sampler_border_color    // transparent_black | opaque_black | opaque_white   (clamp_border only)
sg::compare_op              // never|less|equal|less_equal|greater|not_equal|greater_equal|always (comparison/shadow sampler)
// two ways in (see the bind path): STATIC = named_sampler on create_binding_group_layout (baked into the pipeline layout's root sig);
//                                  DYNAMIC = named_sampler on create_binding_group (written to a sampler heap).
// dx12 only; a cube UAV analogue — samplers live in their own descriptor heap + root table.
```

## bindings & compiled shaders — reflection data model  (see docs/concepts/bindings.md)

```cpp
#include <shaped-graphics/binding.hh>
sg::binding_type            // uniform_buffer | read{only,write}_structured_buffer | read{only,write}_raw_buffer
                            //   | read{only,write}_texture | sampler | acceleration_structure   (replaces D3D_SHADER_INPUT_TYPE)
sg::binding                 // { cc::string name; u32 set, index, count; binding_type type; cc::optional<isize> block_size }
                            //   (set,index) = SPIR-V set/binding / WGSL @group/@binding; count 0 = unbounded
sg::access_of(type)         // view_class the type expects   |  sg::shape_of(type) // view_shape it expects
sg::accepts(type, raw_view) // bool — a bound view satisfies a binding of this type (access & shape match)
sg::is_sampler(type)        // bool — a sampler binding (bound as a sampler, not a view)

#include <shaped-graphics/compiled_shader.hh>
sg::shader_stage            // vertex | tessellation_control(hull) | tessellation_evaluation(domain) | geometry | fragment | compute | raygen | closest_hit | any_hit | miss | intersection | callable
sg::is_raytracing_stage(s)  // bool — one of the six RT stages;  sg::is_compute_stage(s) — the compute stage
sg::shader_format           // dxil | spirv | metal_lib — which backend consumes the blob (ctx.accepts_shader_format(f))
// sg only CONSUMES compiled shaders. Producing one — packages, compilation, hot reload — is
// shaped-shader-library's job; docs/shaders.md is the front door for the whole shader system.
sg::compiled_shader         // { stage; format; entry_point; cc::vector<byte> bytecode; cc::vector<binding> bindings;
                            //   cc::optional<compute_dimensions> workgroup_size; compiler_info compiler }  — value type
sg::compiled_shader_handle  // std::shared_ptr<compiled_shader const>
// data model only: no compiler yet (construct by hand / future loader)
```

## bind path — group layout / pipeline layout / pipeline / group + compute dispatch  (dx12 real; vulkan stubs)

```cpp
#include <shaped-graphics/binding_group_layout.hh>   // + pipeline_layout.hh / compute_pipeline.hh / binding_group.hh
sg::binding_group_layout / sg::pipeline_layout / sg::compute_pipeline / sg::binding_group  // abstract; backend subclasses; *_handle = shared_ptr<T const>
sg::named_view              // { cc::string name; raw_view view }  — input to create_binding_group (a typed view converts)
sg::named_sampler           // { cc::string name; sampler sampler }  — name-matched: static (on group layout) or dynamic (on group)
sg::bound_sampler           // { binding binding; sampler sampler }  — register-bound static sampler, attached to a pipeline_layout
sg::max_binding_groups      // int — hard cap on pipeline_layout group slots (== cmd.compute.bind_group's `set`)
sg::pipeline_layout_description   // { small_vector<binding_group_layout_handle, max_binding_groups> groups; cc::vector<bound_sampler> static_samplers }  — groups ordered; index = bind slot
sg::compute_pipeline_description  // { compiled_shader const& shader; pipeline_layout_handle layout; pinned_data<byte const> cached_pipeline={} }
compute_pipeline.cached_pipeline_data()  // -> pinned_data<byte const> — backend's serialized PSO blob; persist + feed back via desc.cached_pipeline (empty if unsupported / accelerator only, NOT in the cache key)
// layouts + pipelines are schemas/PSOs (not lifetime-scoped) -> the RAW ctx.uncached scope. Prefer ctx.cached (below).
ctx.uncached.create_binding_group_layout(span<binding const>, span<named_sampler const> statics={})  // -> binding_group_layout_handle (name-matched statics baked into the root sig by the pipeline layout; + try_ twin)
ctx.uncached.create_pipeline_layout({.groups={gl0, gl1, ...}, .static_samplers={...}})  // -> pipeline_layout_handle (ordered group layouts + extra register-bound static samplers -> one root signature; + try_ twin)
ctx.uncached.create_compute_pipeline({.shader=, .layout=})               // -> compute_pipeline_handle (.layout is a pipeline_layout; blocking build; throws sg::pipeline_creation_exception; + try_ twin)
ctx.uncached.create_raster_pipeline({.layout=, .vertex_shader=, .fragment_shader=, .vertex_input=, .color_targets={{...}}, ...})  // -> raster_pipeline_handle (blocking build; throws; + try_ twin). No ctx.cached yet.
// binding_group IS a per-scope descriptor allocation -> ctx.persistent / ctx.transient (instantiates a group layout):
ctx.persistent.create_binding_group(group_layout, span<named_view const>, span<named_sampler const> dyn={})  // -> binding_group_handle (validated vs group layout; + try_ twin)
ctx.transient.create_binding_group(group_layout, span<named_view const>, span<named_sampler const> dyn={})   // -> binding_group_handle per-epoch (ring-allocated); layouts/pipeline come from ctx.uncached (+ try_ twin)
// recording (on a command_list, via the cmd.compute scope):
cmd.compute.bind_pipeline(pipeline)      // void — active pipeline (caches its workgroup size + bound pipeline layout)
cmd.compute.bind_group(set, group)       // void — bind a binding_group at slot `set` (indexes the pipeline layout's groups)
cmd.compute.dispatch_groups(x, y, z)     // void — dispatch x*y*z workgroups
cmd.compute.dispatch_threads(x, y, z)    // void — dispatch ceil(threads / workgroup_size) groups per axis
cmd.compute.declare_array_buffer_access(name, elements)  // void — per-element access for a buffer array/bindless binding
cmd.compute.declare_array_texture_access(name, elements) // void — same for a texture array (elements also carry a layout)
                                                         // (scalar bindings are inferred; arrays can't be — declare them)

// raster_pipeline — a graphics PSO. Owns its shaders; formats/state baked in (must match the rendering scope). Draws via cmd.raster (above).
sg::raster_pipeline_description   // { pipeline_layout_handle layout; compiled_shader vertex_shader; optional<compiled_shader> fragment_shader;
                                  //   optional<compiled_shader> tessellation_control_shader/tessellation_evaluation_shader (both-or-neither, need patch_list); optional<compiled_shader> geometry_shader;
                                  //   vertex_input_layout vertex_input; primitive_topology topology=triangle_list; int patch_control_points=0 (1..32, patch_list only); rasterization_state; depth_stencil_state;
                                  //   small_vector<color_target_state,8> color_targets; pixel_format depth_stencil_format=undefined; int sample_count=1; pinned_data cached_pipeline={} }
sg::color_target_state            // { pixel_format format; optional<blend_state> blend={}; color_write_mask write_mask=all }  — one color target's PSO state
sg::vertex_input_layout           // { small_vector<vertex_input_slot,8> slots; vector<vertex_attribute> attributes }; static create<Vs...>() derives one slot per type
                                  //   via a sg::vertex_layout_of<V> specialization (static vertex_type_layout get()). vertex_attribute { string semantic; u32 semantic_index; vertex_attribute_format format; isize offset; int slot }
// state vocab (backend-neutral enums; primitive_topology.hh / rasterization_state.hh / blend_state.hh / depth_stencil_state.hh):
//   primitive_topology {point_list,line_list,line_strip,triangle_list,triangle_strip,patch_list}  fill_mode{solid,wireframe}  cull_mode{none,front,back}  front_face{counter_clockwise,clockwise}
//   blend_factor / blend_op / color_write_mask (r|g|b|a|all bit flags)  stencil_op  depth_stencil_state reuses sg::compare_op (from sampler.hh)
//   vertex_attribute_format {f32,vec2f,vec3f,vec4f, i32.., u32.., rgba8_unorm, rgba8_uint}   index_format {uint16, uint32}
raster_pipeline.cached_pipeline_data()  // -> pinned_data<byte const> — serialized PSO blob; persist + feed back via desc.cached_pipeline (empty if unsupported)
// Access is inferred from each op (upload⇒copy_write, dispatch⇒bound views' access); no public
// declare_access. Concurrent command lists are fine — each takes a tracking slot. See docs/concepts/barriers.md.
```

## acceleration structures — ray-tracing blas / tlas  (see docs/concepts/acceleration-structures.md)

```cpp
#include <shaped-graphics/acceleration_structure.hh>   // resources + input structs (also via command_list.raytracing.hh)
sg::blas_handle   // std::shared_ptr<sg::blas const>   — bottom-level (one mesh's triangles or AABBs); persistent
sg::tlas_handle   // std::shared_ptr<sg::tlas const>   — top-level (instances of blas); a tlas keeps its blases alive
// input structs (value types; build-input buffers need buffer_usage::accel_structure_build_input):
sg::blas_triangles { vertices(float3), vertex_count/stride/offset; optional indices+count+offset+index_type;
                     optional transform buffer(3x4 row-major)+offset; is_opaque=true }
sg::blas_aabbs     { aabbs(6 floats each), aabb_count/stride/offset; is_opaque=true }
sg::tlas_instance  { blas_handle blas; float transform[12] ROW-MAJOR 3x4 (transform[r*4+c], not tg col-major);
                     u32 instance_id:24; u32 hit_group_offset:24; u8 mask=0xFF; instance_cull_mode; optional opaque_override }
sg::accel_build_flags   // none/fast_trace(default)/fast_build/allow_update/allow_compaction/minimize_memory  (|, has_flag)
sg::index_format        // uint16 | uint32  — index-buffer element width (shared with draw's bind_index_buffer)
sg::instance_cull_mode  // back(default) | front | none

// recording (on a command_list, via the cmd.raytracing scope). Sizes+allocates the persistent result from a
// prebuild query, records the build with transient scratch, returns a persistent handle. Throws sg::allocation_exception.
cmd.raytracing.is_supported()                    // bool — backend/device supports ray tracing? gate builds/tests on it
cmd.raytracing.build_blas(span<blas_triangles const>, flags=fast_trace)  // -> blas_handle
cmd.raytracing.build_blas(span<blas_aabbs const>,     flags=fast_trace)  // -> blas_handle  (a blas is triangles OR aabbs)
cmd.raytracing.build_tlas(span<tlas_instance const>,  flags=fast_trace)  // -> tlas_handle  (each blas must be built first)
// blas/tlas: storage() -> raw_buffer_handle; size_in_bytes(); geometry_count()/instance_count(); build_flags();
//   allows_update(); is_expired()/expire()/add_finalizer(). dx12 real (WARP); vulkan is_supported()==false + stubs.
tlas.as_view()  // -> tlas_view — bind the TLAS as HLSL RaytracingAccelerationStructure (inline RT / RayQuery)
```

## raytracing pipeline + shader table + dispatch_rays  (dx12 real on WARP; see docs/concepts/raytracing-pipeline.md)

```cpp
#include <shaped-graphics/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing_shader_table.hh>
// each RT shader is its own single-entry lib_6_x compiled_shader (stage raygen/miss/closest_hit/any_hit/intersection/callable)
sg::hit_shader { optional<compiled_shader> closest_hit, any_hit, intersection; }  // intersection present ⇒ procedural hit group
sg::raytracing_pipeline_description { pipeline_layout_handle layout;              // global root signature (one, no local root sigs)
    vector<compiled_shader> raygen_shaders, miss_shaders, callable_shaders; vector<hit_shader> hit_shaders;
    u32 max_recursion_depth=1; isize max_payload_size=0, max_attribute_size=8; pinned_data cached_pipeline; }
//   phase 1 — register in the pipeline, returns a *_shader_handle:
desc.add_raygen_shader(compiled_shader)  // -> raygen_shader_handle    (also add_miss_shader / add_callable_shader)
desc.add_hit_shader(hit_shader)          // -> hit_shader_handle
ctx.uncached.create_raytracing_pipeline(desc)      // -> raytracing_pipeline_handle (throws; sync escape hatch)
ctx.cached.acquire_raytracing_pipeline(desc)       // -> async_raytracing_pipeline  (memoized, async build)

sg::raytracing_shader_table_description { raytracing_pipeline_handle pipeline;
    vector<raygen_shader_handle> raygen; vector<miss_shader_handle> miss; vector<hit_shader_handle> hit; vector<callable_shader_handle> callable; }
//   phase 2 — place a handle in the table, returns a *_index (what HLSL TraceRay / dispatch_rays address):
tbl.add_raygen_shader(raygen_shader_handle)  // -> raygen_index         (also add_miss_shader / add_hit_shader / add_callable_shader)
ctx.uncached.create_raytracing_shader_table(tbl)   // -> raytracing_shader_table_handle (persistent, uncached, ties to one pipeline)

// recording (on a command_list, via cmd.raytracing). Binds through the compute root signature.
cmd.raytracing.bind_pipeline(raytracing_pipeline const&)          // void — sets the DXR state object + global root signature
cmd.raytracing.bind_group(int set, binding_group const&)         // void — like compute; bind a tlas here (surfaces accel_read)
cmd.raytracing.dispatch_rays(table, raygen_index, w, h=1, d=1)   // void — traces w*h*d rays (product <= 2^30)
```

## cached layouts + pipelines — the built-in cache  (ctx.cached / pipeline_cache)

```cpp
#include <shaped-graphics/context.cached.hh>   // (via context.hh) — the ctx.cached scope
#include <shaped-graphics/pipeline_cache.hh>   // the cache itself
// Every context has a built-in pipeline_cache (default in-memory tiers installed). "acquire" = get-or-create.
ctx.cached.acquire_binding_group_layout(span<binding const>, static_samplers={}) // -> binding_group_layout_handle  SYNC; (bindings, static_samplers) keyed => one shared handle
ctx.cached.acquire_pipeline_layout({.groups={gl0, ...}})       // -> pipeline_layout_handle  SYNC; keyed on the ordered group-layout identities => one shared handle
ctx.cached.acquire_compute_pipeline({.shader=, .layout=})      // -> sg::async_compute_pipeline  async PSO build; identical (shader, pipeline layout) => one node
                                                               //   drive: cc::async_blocking_get_singlethreaded(p) -> compute_pipeline_handle; or poll p->is_ready()/try_value()
ctx.cached.acquire_raytracing_pipeline(rt_desc)               // -> sg::async_raytracing_pipeline  async state-object build; keyed on all shaders + layout + limits
ctx.cached.cache()                                             // -> pipeline_cache&  to install extra tiers / run bookkeeping
// keys = hash128 over the logical args (group layout: bindings + static samplers; pipeline layout: ordered group-layout
//   identities; compute pipeline: shader bytecode+entry+signature + pipeline-layout handle identity).
// For full dedup, acquire the group layouts THROUGH the cache, then the pipeline layout, then the pipeline.
// Threading: the async build calls the backend from a pool worker — safe where the backend allows concurrent
// pipeline creation (dx12 device creates are free-threaded). On single_threaded, install NO pool and drive inline.
pipeline_cache pc;                                            // standalone use (acquire_* take a context&)
pc.acquire_binding_group_layout(ctx, bindings);  pc.acquire_pipeline_layout(ctx, {.groups={gl}});  pc.acquire_compute_pipeline(ctx, desc);
pc.add_default_in_memory_providers(max=4096);  pc.add_binding_group_layout_provider(p);  pc.apply_bookkeeping();
// TODO: graphics / raytracing pipeline caching once those pipeline types land in sg.
```

## render routines — reusable GPU-work units  (see docs/render-routines.md)

```cpp
#include <shaped-graphics/render_routine.hh>
// A routine is a per-context singleton reached BY TYPE. Derive from the CRTP base:
class my_routine : public sg::render_routine<my_routine> { ... };
// protected virtuals (all default to no-ops) — three-phase init, split so async compiles start early:
void init_once(sg::context& ctx)          // first init only, NEVER on reload — persistent, shader-independent work
void init_declare(sg::context& ctx)       // first init + after every reload — acquire shaders/pipelines; NO GPU work/recording
void init_materialize(sg::command_list&)  // first init + after every reload — record GPU init work
// static entry points the CRTP adds (all reach the per-context instance by type — no handle, no registration):
my_routine::acquire(cmd)                   // -> my_routine const&  — lazily create in cmd.context().routines, init (declare+materialize), return
my_routine::prewarm(ctx)                   // void     — create + init_once/init_declare only (before a command list; async compiles fan out on the pool)
my_routine::evict(ctx)                     // void     — drop this routine's instance + its cached GPU state
// acquire memoizes the instance per thread (weak, so it never keeps a routine alive past evict/clear/shutdown).
// re-init is driven by sg::reload_generation() (process-global); init_once state survives reloads.

#include <shaped-graphics/routine_registry.hh>   // (via context.hh) — the ctx.routines scope; type-keyed access is private to the CRTP
ctx.routines.clear()                       // void     — drop all (VRAM pressure / context switch); runs automatically on shutdown
// Per-context: a routine's cached GPU state dies with the context that built it — never stale across contexts.

#include <shaped-graphics/reload_generation.hh>
sg::reload_generation()                    // -> u64   — process-global "content-derived state invalidated" counter (read)
sg::signal_reload()                        // void     — bump it (the shader library calls this on hot reload)
```

## memory placement — heaps & alloc-info  (stub)

```cpp
#include <shaped-graphics/fwd.hh>
sg::lifetime_scope                      // persistent | transient  (hard lifetime contract; transient expires at epoch retire)
#include <shaped-graphics/allocation_info.hh>
sg::allocation_info                     // value type: where a resource's memory lives (cheap to copy)
ai.heap                                 // memory_heap_handle — null = dedicated / self-allocating
ai.offset / ai.size_in_bytes            // isize — placement within `heap` (ignored when dedicated)
ai.scope                                // sg::lifetime_scope
ai.is_dedicated()                       // bool — heap == nullptr (owns its allocation; "committed" in dx12)
ai.is_placed()                          // bool — heap != nullptr (sub-allocated into a shared heap)

#include <shaped-graphics/memory_heap.hh>
sg::memory_requirements                 // { isize alignment_in_bytes; isize size_in_bytes; }  (backend-reported)
// abstract, immutable; a backend subclasses it. shared via memory_heap_handle = shared_ptr<memory_heap const>
h.size_in_bytes()                       // isize — total underlying allocation
h.memory_requirements_for_buffer(size, usage)         // -> memory_requirements (alignment + actual occupied size)
h.acquire_allocation_for_buffer(size, usage, offset)  // -> allocation_info, const (validates offset alignment/bounds, mints handle back to h; no tracking)
// protected pure-virtual query_buffer_requirements(size, usage) is the backend hook both public methods build on
// flow: query reqs -> your allocator picks offset -> h.acquire_allocation_for_*(...) -> pass allocation_info to create_*
// create_raw_buffer takes the allocation_info, but only dedicated (null heap) works today — placement asserts.
// NOT yet wired: no context.create_memory_heap; placed allocations not implemented in the backends yet
```

## backends — subclass the abstract sg types

```cpp
// context/command_list/buffer are abstract; backends derive directly (no separate bridge layer):
sg::backend::dx12::dx12_context   : sg::context        // + dx12_command_list, dx12_buffer
sg::backend::vulkan::vulkan_context : sg::context      // + vulkan_command_list, vulkan_buffer
// creation: sg::create_dx12_context / sg::create_vulkan_context  (declared in the backend headers)
// backend-typed API (no downcasts when you hold the concrete context):
dctx.create_dx12_buffer(size, usage)      // -> cc::result<dx12_buffer_handle>  (shared_ptr<dx12_buffer const>)
dctx.create_dx12_command_list()           // -> cc::result<std::unique_ptr<dx12_command_list>>
// the sg::context virtuals are thin forwarders to these backend-typed methods
// escape hatch: dynamic_cast<sg::backend::vulkan::vulkan_context*>(ctx.get()) — "here be dragons"
```
