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
// command_list has NO handle: it's a move-only temporary — std::unique_ptr<sg::command_list>,
// passed around by reference (command_list&). record once, submit once, not reused.
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
```

## context — mutable driver / factory

```cpp
#include <shaped-graphics/context.hh>
ctx.backend()                                      // sg::backend_kind (coarse tag, not identity)
ctx.threading()                                    // sg::thread_model — which ops are concurrency-safe
ctx.is_device_lost() / ctx.device_loss_reason()    // bool / string_view — sticky device-lost status (see Error handling above)
ctx.create_command_list()                          // -> std::unique_ptr<command_list> (already recording); infallible (throws only on device loss)
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
ctx.upload.set_async_window_size(bytes)            // void — resize the async staging window (x3 buffered); copy actor adopts it between windows; default 16 MiB
ctx.upload.set_inline_budget(bytes)                // void — resize the inline (cmd.upload) ring; applied at the next advance_epoch; default 16 MiB
ctx.download.bytes_from_buffer(buf, offset_in_bytes, size)    // -> sg::bytes_future — ASYNC read buf back on the copy queue (needs copy_src); read auto-waits on the last writer, a later writer auto-waits on the read; drop the future to cancel; size 0 = ready empty future
ctx.download.data_from_buffer<T>(buf, off_in_elements, count) // -> sg::data_future<T>; offset AND count in ELEMENTS of T. See bytes_from_buffer
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
sg::pipeline_creation_exception  // binding_layout / compute_pipeline build failure; .entry_point()
sg::binding_group_exception      // binding_group wiring error (unknown/missing binding, kind mismatch) or descriptor exhaustion
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
cmd.download.bytes_from_buffer(buf, offset_in_bytes, size)    // -> sg::bytes_future (needs copy_src); size 0 = ready empty future
cmd.download.data_from_buffer<T>(buf, off_in_elements, count) // -> sg::data_future<T>; offset AND count in ELEMENTS of T
cmd.copy.buffer_bytes_region({.src, .dst, .size_in_bytes, .src_offset_in_bytes=0, .dst_offset_in_bytes=0}) // void — device→device buffer copy (src needs copy_src, dst needs copy_dst); size 0 = no-op
cmd.copy.buffer_data_region<T>({.src, .dst, .count, .src_offset=0, .dst_offset=0}) // void — typed convenience (count + offsets in elements of T; like a subspan)
// cmd.upload/download = INLINE (recorded in this list); ctx.upload/download = ASYNC (copy queue, off the
// frame path — for bulk streaming/readback). See docs/concepts/{upload,download}.async.md.
// inline path: copy is recorded here; the download future is delivered by a separate actor after the
// submitted list finishes on the GPU (no advance_epoch needed — but advance_epoch* / wait_for_idle do NOT
// guarantee delivery either; use ctx.wait_for(future)). Uploading + downloading + copying the SAME buffer works in ONE list —
// the access tracker orders them (see docs/concepts/barriers.md). Self-copy needs non-overlapping ranges.
// vulkan transfer is a TODO stub.
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
// storage (UAV) — needs readwrite_texture; single mip; not on MS (a cube UAV is a 2D array):
tex.as_readwrite_view({.mip=1})                // -> texture_readwrite_view  (whole, natural dimension)
//   read_write_params fields: .mip always; .slices (arrays/cubes); .depth_slices (3D, the W/Z axis)
tex.as_readwrite_2d_view({.slice=3,.mip=0})    // array/cube -> Texture2D    tex.as_readwrite_1d_view({.slice=3})
// typedefs: texture_1d/2d/3d, texture_cube, texture_1d_array/2d_array/cube_array,
//           texture_2d_ms/2d_array_ms/cube_ms/cube_array_ms
// bind a texture view in a compute dispatch → it auto-transitions to shader_read (SRV) / storage (UAV).
// NOTE: SRV/UAV only — render_target/depth_stencil views + samplers + texture upload/download/copy remain future work.
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
sg::view_class               // uniform | readonly | readwrite   (access; mirrors buffer_usage / texture_usage)
sg::view_shape               // uniform_block | structured | raw | texture   (layout)
sg::raw_view                 // erased tagged struct every typed view converts into — what backends consume
v.to_raw()  /  (implicit)    // sg::raw_view  { access, shape, buffer|texture, offset/size/... | format+range }
// backends switch on (access, shape) to build the native descriptor (SRV/UAV/CBV/texture SRV/UAV)
// deferred: render_target/depth_stencil views, samplers, and texel buffers (typed linear buffers)
```

## bindings & compiled shaders — reflection data model  (see docs/concepts/bindings.md)

```cpp
#include <shaped-graphics/binding.hh>
sg::binding_type            // uniform_buffer | read{only,write}_structured_buffer | read{only,write}_raw_buffer
                            //   backend-agnostic reflection kind (replaces D3D_SHADER_INPUT_TYPE); +sampler/texture/accel later
sg::binding                 // { cc::string name; u32 set, index, count; binding_type type; cc::optional<isize> block_size }
                            //   (set,index) = SPIR-V set/binding / WGSL @group/@binding; count 0 = unbounded
sg::access_of(type)         // view_class the type expects   |  sg::shape_of(type) // view_shape it expects
sg::accepts(type, raw_view) // bool — a bound view satisfies a binding of this type (access & shape match)

#include <shaped-graphics/compiled_shader.hh>
sg::shader_stage            // vertex | fragment | compute (+ more later)
sg::shader_format           // dxil | spirv | metal_lib — which backend consumes the blob
sg::compiled_shader         // { stage; format; entry_point; cc::vector<byte> bytecode; cc::vector<binding> bindings;
                            //   cc::optional<compute_dimensions> workgroup_size; compiler_info compiler }  — value type
sg::compiled_shader_handle  // std::shared_ptr<compiled_shader const>
// data model only: no compiler yet (construct by hand / future loader)
```

## bind path — layout / pipeline / group + compute dispatch  (dx12 real; vulkan stubs)

```cpp
#include <shaped-graphics/binding_layout.hh>   // + compute_pipeline.hh / binding_group.hh
sg::binding_layout / sg::compute_pipeline / sg::binding_group   // abstract; backend subclasses; *_handle = shared_ptr<T const>
sg::named_view              // { cc::string name; raw_view view }  — input to create_binding_group (a typed view converts)
sg::compute_pipeline_description  // { compiled_shader const& shader; binding_layout_handle layout }
// creation (on ctx.persistent -> persistent lifetime_scope; the context virtuals take the scope explicitly):
ctx.persistent.create_binding_layout(span<binding const>)                 // -> binding_layout_handle   (the set schema; throws sg::pipeline_creation_exception; + try_ twin)
ctx.persistent.create_compute_pipeline({.shader=, .layout=})              // -> compute_pipeline_handle (throws sg::pipeline_creation_exception; + try_ twin)
ctx.persistent.create_binding_group(layout, span<named_view const>)       // -> binding_group_handle    (validated vs layout; throws sg::binding_group_exception; + try_ twin)
ctx.transient.create_binding_group(layout, span<named_view const>)        // -> binding_group_handle    per-epoch (ring-allocated); layout/pipeline stay persistent (+ try_ twin)
// recording (on a command_list, via the cmd.compute scope):
cmd.compute.bind_pipeline(pipeline)      // void — active pipeline (caches its workgroup size)
cmd.compute.bind_group(set, group)       // void — bind a binding_group to descriptor set `set`
cmd.compute.dispatch_groups(x, y, z)     // void — dispatch x*y*z workgroups
cmd.compute.dispatch_threads(x, y, z)    // void — dispatch ceil(threads / workgroup_size) groups per axis
cmd.compute.declare_array_buffer_access(name, elements)  // void — per-element access for a buffer array/bindless binding
cmd.compute.declare_array_texture_access(name, elements) // void — same for a texture array (elements also carry a layout)
                                                         // (scalar bindings are inferred; arrays can't be — declare them)
// Access is inferred from each op (upload⇒copy_write, dispatch⇒bound views' access); no public
// declare_access. Concurrent command lists are fine — each takes a tracking slot. See docs/concepts/barriers.md.
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
