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
sg::buffer_handle         // std::shared_ptr<sg::buffer>         — shared-immutable resource
// command_list has NO handle: it's a move-only temporary — std::unique_ptr<sg::command_list>,
// passed around by reference (command_list&). record once, submit once, not reused.
```

## Enums

```cpp
#include <shaped-graphics/types.hh>
sg::backend_kind          // dx12, vulkan, metal, webgpu, opengl, webgl
sg::thread_model          // single_threaded | multi_threaded (see docs/concepts/threading.md)
sg::buffer_usage          // bit flags: none/copy_src/copy_dst/vertex/index/uniform/storage
a | b                     // combine usages
sg::has_flag(usage, flag) // bool — every bit of `flag` set in `usage`
```

## context — mutable driver / factory

```cpp
#include <shaped-graphics/context.hh>
ctx.backend()                                      // sg::backend_kind (coarse tag, not identity)
ctx.threading()                                    // sg::thread_model — which ops are concurrency-safe
ctx.create_command_list()                          // -> cc::result<std::unique_ptr<command_list>> (already recording)
ctx.create_buffer(size_in_bytes, usage)            // -> cc::result<buffer_handle>  (size>=0; 0 = empty, no alloc)
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
// recording API (copy/upload/download buffer, ...) — pure-virtual — lands with the first milestone
```

## buffer — GPU-resident, immutable shape  (abstract)

```cpp
#include <shaped-graphics/buffer.hh>
// abstract; a backend subclasses it. protected ctor: buffer(size_in_bytes, usage)  (size 0 = empty)
b.size_in_bytes()                  // isize   (inline, cheap — no virtual call)
b.usage()                          // sg::buffer_usage
// shape metadata (_size_in_bytes/_usage) is protected in the base; backend buffers inherit it
```

## memory placement — heaps & alloc-info  (stub)

```cpp
#include <shaped-graphics/allocation_info.hh>
sg::allocation_scope                    // persistent | transient  (transient = recycle hint, still hazard-tracked)
sg::allocation_info                     // value type: where a resource's memory lives (cheap to copy)
ai.heap                                 // memory_heap_handle — null = dedicated / self-allocating
ai.offset / ai.size_in_bytes            // isize — placement within `heap` (ignored when dedicated)
ai.scope                                // sg::allocation_scope

#include <shaped-graphics/memory_heap.hh>
sg::memory_requirements                 // { isize alignment_in_bytes; isize size_in_bytes; }  (backend-reported)
// abstract; a backend subclasses it. shared via memory_heap_handle (enable_shared_from_this)
h.size_in_bytes()                       // isize — total underlying allocation
h.memory_requirements_for_buffer(size, usage)         // -> memory_requirements (alignment + actual occupied size)
h.acquire_allocation_for_buffer(size, usage, offset)  // -> allocation_info (validates offset alignment/bounds, mints handle back to h)
// protected pure-virtual query_buffer_requirements(size, usage) is the backend hook both public methods build on
// flow: query reqs -> your allocator picks offset -> h.acquire_allocation_for_*(...) -> pass allocation_info to create_*
// NOT yet wired: no context.create_memory_heap, create_buffer doesn't take allocation_info yet
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
