# shaped-graphics cheat sheet

Graphics-API wrapper. Namespace `sg`. Depends on clean-core + typed-geometry. Headers are
included by full path from `src/`: `#include <shaped-graphics/<name>.hh>`.

> **Scope note:** this sheet covers the small surface that exists today. The **dx12** backend is
> real (device / command list / GPU buffer); the sg core abstract API and the **vulkan** backend are
> still stubs. Fallible creates return `cc::result`.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

How to read this: each block leads with the include; one symbol per line with a trailing
comment giving the return type / intuition.

---

## Handles & types

```cpp
#include <shaped-graphics/fwd.hh>
sg::context_handle        // std::shared_ptr<sg::context>        — shared, long-lived driver
sg::buffer_handle         // std::shared_ptr<sg::buffer const>   — shared-immutable resource
// command_list has NO handle: it's a move-only temporary — std::unique_ptr<sg::command_list>,
// passed around by reference (command_list&). record once, submit once, not reused.
```

## bytes_future / bytes_waiter — download results

```cpp
#include <shaped-graphics/bytes_future.hh>
sg::bytes_future                    // returned by cmd.download.bytes_from_buffer; holds {span, pin, waiter}
f.is_valid()                        // bool — backed by a real download (vs default-constructed)
f.is_ready()                        // bool — polls the waiter; true once the bytes are valid
f.try_get_bytes()                   // -> cc::optional<cc::span<cc::byte const>>  (polls; nullopt until ready)
f.wait_get_bytes()                  // -> cc::optional<...>  blocks until ready; nullopt if not yet submitted
sg::data_future<T>                  // typed wrapper: try_get_data()/wait_get_data() -> cc::optional<cc::span<T const>>
sg::bytes_waiter                    // abstract poll handle a backend subclasses; sg::ready_bytes_waiter = ready-on-construction
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
ctx.create_command_list()                          // -> cc::result<std::unique_ptr<command_list>> (already recording)
ctx.persistent.create_buffer(size, usage, alloc={}) // -> cc::result<buffer_handle>  (size>=0; 0 = empty, no alloc)
                                                   //   resource creation lives on the lifetime scope (sg::context_persistent_scope)
                                                   //   alloc defaults to a dedicated allocation_info; placed (heap) not impl yet
ctx.submit_command_list(std::move(cmd))            // -> submission_token — consumes cmd (submit once; same epoch it opened in)
ctx.drop_command_list(std::move(cmd))              // void — consumes cmd; == letting it leave scope (same epoch)
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
ctx.is_submission_complete(token)       // bool — has that one command list finished?
// command lists cannot span epochs (submit/drop in the epoch opened in — CC_ASSERT-enforced)
// on multi_threaded backends: create/submit/drop are concurrency-safe; advance_*/wait_*/shutdown are NOT
cmd.created_in_epoch()                  // sg::epoch — the epoch this command list was opened in
buf->add_finalizer([]{ ... })           // void — runs after the GPU handle is freed AND no longer in flight
```

## command_list — records GPU work  (abstract)

```cpp
#include <shaped-graphics/command_list.hh>
// abstract; a backend subclasses it (protected ctor). obtained via ctx.create_command_list()
// -> std::unique_ptr; passed by reference (command_list&). record once, submit once, not reused.
cmd.upload.bytes_to_buffer(buf, bytes, offset=0)     // void — stage host bytes into buf (needs copy_dst); empty = no-op
cmd.upload.data_to_buffer(buf, range, offset=0)      // void — typed convenience (trivially-copyable contiguous range)
cmd.download.bytes_from_buffer(buf, offset, size)    // -> sg::bytes_future (needs copy_src); size 0 = ready empty future
cmd.download.data_from_buffer<T>(buf, off, count)    // -> sg::data_future<T>
cmd.copy.buffer_bytes_region({.src, .dst, .size_in_bytes, .src_offset_in_bytes=0, .dst_offset_in_bytes=0}) // void — device→device buffer copy (src needs copy_src, dst needs copy_dst); size 0 = no-op
cmd.copy.buffer_data_region<T>({.src, .dst, .count, .src_offset=0, .dst_offset=0}) // void — typed convenience (count + offsets in elements of T; like a subspan)
// inline path: copy is recorded here; the download future is ready after the submitted list finishes on
// the GPU (no advance_epoch needed). dx12 today: uploading + downloading (or copying) the SAME buffer needs
// separate command lists (no barrier system yet — copy inserts a conservative global barrier for now).
// copy within one buffer requires non-overlapping ranges. vulkan transfer is a TODO stub.
```

## buffer — GPU-resident, immutable shape  (abstract)

```cpp
#include <shaped-graphics/buffer.hh>
// abstract; a backend subclasses it. protected ctor: buffer(size_in_bytes, usage)  (size 0 = empty)
b.size_in_bytes()                  // isize   (inline, cheap — no virtual call)
b.usage()                          // sg::buffer_usage
// shape metadata (_size_in_bytes/_usage) is protected in the base; backend buffers inherit it
// view factories — a strongly-typed view onto this buffer (buffer's usage must cover the access):
b.as_uniform_buffer<T>(offset=0)           // -> sg::uniform_view<T>    (CBV/UBO; needs uniform_buffer usage; offset 256-aligned)
                                           //   T is a uniform_element: size multiple of 16, <= 64 KiB (not byte)
b.as_readonly_buffer<T>({.offset=, .size=})// -> sg::readonly_view<T>   (SRV; range in elements of T; default = whole)
b.as_readwrite_buffer<T>({.offset=,.size=})// -> sg::readwrite_view<T>  (UAV; needs readwrite_buffer usage)
b.as_raw_readonly({.offset=,.size=})       // -> readonly_view<byte>    (raw / byte-addressed; range in bytes; default = whole)
b.as_raw_readwrite({.offset=,.size=})      // -> readwrite_view<byte>   (raw / byte-addressed; range in bytes; default = whole)
```

## views — strongly-typed resource views  (see docs/concepts/views.md)

```cpp
#include <shaped-graphics/views.hh>
sg::view_element<T>          // concept: T is `byte`, or sizeof(T) % 4 == 0 (GPUs load DWORD-aligned)
sg::uniform_element<T>       // concept: view_element + size multiple of 16 and <= 64 KiB (excludes byte)
sg::uniform_view<T>          // uniform block of T   (cbuffer/UBO)          — view_class::uniform
sg::readonly_view<T>         // read array of T      (SRV / read SSBO)      — view_class::readonly  (T=byte → raw)
sg::readwrite_view<T>        // rw array of T        (UAV / rw SSBO)        — view_class::readwrite (T=byte → raw)
// each holds a buffer_handle + range; pure value (no GPU alloc). Made via buffer.as_*() above.
sg::view_class               // uniform | readonly | readwrite   (access; mirrors buffer_usage)
sg::view_shape               // uniform_block | structured | raw (layout; derived from T)
sg::raw_view                 // erased tagged struct every typed view converts into — what backends consume
v.to_raw()  /  (implicit)    // sg::raw_view  { access, shape, buffer, offset/size/element_count/stride }
// backends switch on (access, shape) to build the native descriptor  (name raw_view is TBD)
// buffer views only today; texture/texel views (dimension-typed) deferred until sg::texture + sg::format
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

## bind path — layout / pipeline / group + compute dispatch  (abstract; backends stub until the dx12 compute milestone)

```cpp
#include <shaped-graphics/binding_layout.hh>   // + compute_pipeline.hh / binding_group.hh
sg::binding_layout / sg::compute_pipeline / sg::binding_group   // abstract; backend subclasses; *_handle = shared_ptr<T const>
sg::named_view              // { cc::string name; raw_view view }  — input to create_binding_group (a typed view converts)
// creation (on ctx.persistent; returns cc::result; backends CC_UNREACHABLE until implemented):
ctx.persistent.create_binding_layout(span<binding const>)                 // -> binding_layout_handle   (the set schema)
ctx.persistent.create_compute_pipeline(compiled_shader const&, layout)    // -> compute_pipeline_handle
ctx.persistent.create_binding_group(layout, span<named_view const>)       // -> binding_group_handle    (validated vs layout)
// recording (on a command_list, via the cmd.compute scope):
cmd.compute.bind_pipeline(pipeline)      // void — make it the active compute pipeline
cmd.compute.bind_group(set, group)       // void — bind a binding_group to descriptor set `set`
cmd.compute.dispatch(x, y, z)            // void — dispatch x*y*z workgroups
```

## memory placement — heaps & alloc-info  (stub)

```cpp
#include <shaped-graphics/allocation_info.hh>
sg::allocation_scope                    // persistent | transient  (hard lifetime contract; transient expires at epoch retire)
sg::allocation_info                     // value type: where a resource's memory lives (cheap to copy)
ai.heap                                 // memory_heap_handle — null = dedicated / self-allocating
ai.offset / ai.size_in_bytes            // isize — placement within `heap` (ignored when dedicated)
ai.scope                                // sg::allocation_scope
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
// create_buffer takes the allocation_info, but only dedicated (null heap) works today — placement asserts.
// NOT yet wired: no context.create_memory_heap; placed allocations not implemented in the backends yet
```

## backends — subclass the abstract sg types

```cpp
// context/command_list/buffer are abstract; backends derive directly (no separate bridge layer):
sg::backend::dx12::dx12_context   : sg::context        // + dx12_command_list, dx12_buffer
sg::backend::vulkan::vulkan_context : sg::context      // + vulkan_command_list, vulkan_buffer
// creation: sg::create_dx12_context / sg::create_vulkan_context  (declared in the backend headers)
// backend-typed API (no downcasts when you hold the concrete context):
dctx.create_dx12_buffer(size, usage)      // -> cc::result<dx12_buffer_handle>  (shared_ptr<dx12_buffer>)
dctx.create_dx12_command_list()           // -> cc::result<std::unique_ptr<dx12_command_list>>
// the sg::context virtuals are thin forwarders to these backend-typed methods
// escape hatch: dynamic_cast<sg::backend::vulkan::vulkan_context*>(ctx.get()) — "here be dragons"
```
