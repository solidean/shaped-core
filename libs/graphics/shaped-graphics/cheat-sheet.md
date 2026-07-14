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
ctx.threading()                                    // sg::thread_model — which ops are concurrency-safe
ctx.is_device_lost() / ctx.device_loss_reason()    // bool / string_view — sticky device-lost status (see Error handling above)
ctx.create_command_list()                          // -> std::unique_ptr<command_list> (already recording); infallible (throws only on device loss)
ctx.create_swapchain(swapchain_description = {})   // -> swapchain_handle (throws sg::swapchain_creation_exception / device_lost); see the swapchain section
ctx.try_create_swapchain(swapchain_description = {})  // -> cc::result<swapchain_handle>  (fallible twin)
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
ctx.upload.bytes_to_texture(tex, cc::pinned_data<byte const>, subresource={}, region={})  // void — ASYNC upload tightly-packed pixels into one texture (sub)region (needs copy_dst); later lists reading tex auto-wait
ctx.upload.set_async_window_size(bytes)            // void — resize the async staging window (x3 buffered); copy actor adopts it between windows; default 16 MiB
ctx.upload.set_inline_budget(bytes)                // void — resize the inline (cmd.upload) ring; applied at the next advance_epoch; default 16 MiB
ctx.download.bytes_from_buffer(buf, offset_in_bytes, size)    // -> sg::bytes_future — ASYNC read buf back on the copy queue (needs copy_src); read auto-waits on the last writer, a later writer auto-waits on the read; drop the future to cancel; size 0 = ready empty future
ctx.download.data_from_buffer<T>(buf, off_in_elements, count) // -> sg::data_future<T>; offset AND count in ELEMENTS of T. See bytes_from_buffer
ctx.download.bytes_from_texture(tex, subresource={}, region={}) // -> sg::bytes_future — ASYNC read one texture (sub)region back (needs copy_src), tightly packed
ctx.download.set_async_window_size(bytes)          // void — resize the async readback staging window (x3 buffered); copy actor adopts it between windows; default 16 MiB
ctx.download.set_budget(bytes)                      // void — resize the inline (cmd.download) readback ring; applied at the next advance_epoch (drains the readback actor); default 16 MiB
                                                   //   using any transient resource past its epoch is a hard error (asserts)
ctx.submit_command_list(std::move(cmd))            // -> submission_token — consumes cmd (submit once; same epoch it opened in); throws sg::device_lost_exception on device loss
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
cmd.upload.bytes_to_texture(tex, bytes, subresource={}, region={})  // void — inline upload tightly-packed pixels into one texture (sub)region (needs copy_dst); drives the copy_dst layout barrier; visible to later cmds in the list
cmd.download.bytes_from_buffer(buf, offset_in_bytes, size)    // -> sg::bytes_future (needs copy_src); size 0 = ready empty future
cmd.download.data_from_buffer<T>(buf, off_in_elements, count) // -> sg::data_future<T>; offset AND count in ELEMENTS of T
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
// view factories — a strongly-typed view onto this buffer (buffer's usage must cover the access):
b.as_uniform_buffer<T>(offset=0)           // -> sg::uniform_view<T>    (CBV/UBO; needs uniform_buffer usage; offset 256-aligned)
                                           //   T is a uniform_element: size multiple of 16, <= 64 KiB (not byte)
b.as_readonly_buffer<T>({.offset=, .size=})// -> sg::readonly_view<T>   (SRV; range in elements of T; default = whole)
b.as_readwrite_buffer<T>({.offset=,.size=})// -> sg::readwrite_view<T>  (UAV; needs readwrite_buffer usage)
b.as_raw_readonly({.offset=,.size=})       // -> readonly_view<byte>    (raw / byte-addressed; range in bytes; default = whole)
b.as_raw_readwrite({.offset=,.size=})      // -> readwrite_view<byte>   (raw / byte-addressed; range in bytes; default = whole)
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
// create the raw resource (typed create_texture_2d/... factories come later):
ctx.persistent.create_raw_texture(desc)        // -> raw_texture_handle  (dedicated; throws sg::allocation_exception; + try_ twin)
ctx.transient.create_raw_texture(desc)         // -> raw_texture_handle  (dedicated for now; auto-expires; + try_ twin)
// typed wrapper: shape fixed at compile time; getters gated by concepts (depth() only on 3D, etc.)
sg::texture_2d tex(raw_handle);                // asserts the raw shape matches; tex.raw() -> raw_texture_handle
// Each factory takes a shape-specific param bag (Traits::*_params); ranges are view_range{start,count<0=all}.
// sampled (SRV) — needs readonly_texture usage. Natural dimension:
tex.as_readonly_view({.mips={.start=1}})       // -> texture_readonly_view  (whole; params name only this shape's axes)
//   read_only_params fields: .mips always; .slices (arrays); .cubes (cube arrays)
tex.as_readonly_2d_view({.slice=3})            // array/cube -> Texture2D: one slice/.face/{.cube,.face}
tex.as_readonly_1d_view({.slice=3})            // 1D array -> Texture1D
tex.as_readonly_cube_view({.cube=2})           // cube array -> one TextureCube
tex.as_readonly_2d_array_view({.slices={...}}) // cube / cube array -> Texture2DArray (faces as a flat 2D array)
// storage (UAV) — needs readwrite_texture; single mip; not on MS (a cube UAV is a 2D array):
tex.as_readwrite_view({.mip=1})                // -> texture_readwrite_view  (whole, natural dimension)
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
sg::uniform_view<T>          // uniform block of T   (cbuffer/UBO)          — view_class::uniform
sg::readonly_view<T>         // read array of T      (SRV / read SSBO)      — view_class::readonly  (T=byte → raw)
sg::readwrite_view<T>        // rw array of T        (UAV / rw SSBO)        — view_class::readwrite (T=byte → raw)
// each holds a raw_buffer_handle + range; pure value (no GPU alloc). Made via buffer.as_*() above.
sg::texture_readonly_view    // sampled texture (SRV) — view_class::readonly,  shape texture
sg::texture_readwrite_view   // storage texture (UAV) — view_class::readwrite, shape texture
// each holds { raw_texture_handle, pixel_format, subresource_range }. Made via texture<Traits>.as_*_view().
sg::acceleration_structure_view // ray-tracing TLAS (SRV, VA-addressed) — view_class::acceleration_structure. Via tlas.as_view()
sg::view_class               // uniform | readonly | readwrite | acceleration_structure   (access)
sg::view_shape               // uniform_block | structured | raw | texture | acceleration_structure   (layout)
sg::raw_view                 // erased tagged struct every typed view converts into — what backends consume
v.to_raw()  /  (implicit)    // sg::raw_view  { access, shape, buffer|texture, offset/size/... | format+range }
// backends switch on (access, shape) to build the native descriptor (SRV/UAV/CBV/texture SRV/UAV)
// deferred: texel buffers (typed linear buffers). samplers: see sampler.hh
```

```cpp
// also in views.hh: render-target / depth-stencil target views — NOT shader-facing (do not erase to raw_view)
sg::render_target_view       // color render target (RTV) — made via texture<Traits>.as_render_target_view()
sg::depth_stencil_view       // depth-stencil target (DSV) — via .as_depth_stencil_view()
// each holds { raw_texture_handle, texture_view_dimension, pixel_format, subresource_range } (single mip).
v.texture() v.dimension() v.format() v.range()   // getters
v.width() v.height()         // int — the viewed mip's pixel size (mip-adjusted, >= 1)
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
auto rt = sc->acquire_backbuffer();   // -> render_target_view for the current back buffer (auto-resizes to the window first)
//   ... render into rt this frame (rt.cleared(color) / cmd.raster.render_to({.color_targets = {...}})) ...
sc->present();                        // hand the acquired buffer to the display — EXACTLY once per successful acquire
sc->get_size()                        // tg::vec2i {width, height} — current resolution (tracks auto-resize)
sc->get_width() / sc->get_height()    // int
sc->get_format() sc->get_buffer_count() sc->get_present_mode() sc->is_hdr_enabled() sc->get_native_window_handle()
sc->get_description()                 // swapchain_description const&
// acquire/present throw sg::device_lost_exception on device loss. A bad handle / count / non-renderable format asserts.
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
sg::shader_format           // dxil | spirv | metal_lib — which backend consumes the blob
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
tlas.as_view()  // -> acceleration_structure_view — bind the TLAS as HLSL RaytracingAccelerationStructure (inline RT / RayQuery)
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
                                                               //   drive: cc::async_blocking_get(p) -> compute_pipeline_handle; or poll p->is_ready()/try_value()
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
