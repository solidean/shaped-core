# shaped-graphics cheat sheet

Graphics-API wrapper. Namespace `sg`. Depends on clean-core + typed-geometry. Headers are
included by full path from `src/`: `#include <shaped-graphics/<name>.hh>`.

> **Scope note:** this sheet covers the small surface that exists today. Most of it is stubbed
> (`CC_UNREACHABLE("not implemented yet")`) — the shapes are real, the implementations are not.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

How to read this: each block leads with the include; one symbol per line with a trailing
comment giving the return type / intuition.

---

## Handles & types

```cpp
#include <shaped-graphics/fwd.hh>
sg::context_handle        // std::shared_ptr<sg::context>        — mutable driver
sg::command_list_handle   // std::shared_ptr<sg::command_list>   — mutable driver
sg::buffer_handle         // std::shared_ptr<sg::buffer>         — shared-immutable resource
```

## Enums

```cpp
#include <shaped-graphics/types.hh>
sg::backend_kind          // dx12, vulkan, metal, webgpu, opengl, webgl
sg::buffer_usage          // bit flags: none/copy_src/copy_dst/vertex/index/uniform/storage
a | b                     // combine usages
sg::has_flag(usage, flag) // bool — every bit of `flag` set in `usage`
```

## context — mutable driver / factory  (stubbed)

```cpp
#include <shaped-graphics/context.hh>
ctx.backend()                                      // sg::backend_kind (coarse tag, not identity)
ctx.create_command_list()                          // -> command_list_handle   [stub]
ctx.create_buffer(size_in_bytes, usage)            // -> buffer_handle          [stub]
// you never call sg::create_context — there is none. each backend library provides the factory:
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
sg::create_vulkan_context(vulkan_config = {})      // -> context_handle         [stub]
#include <shaped-graphics/backends/dx12/dx12_context.hh>
sg::create_dx12_context(dx12_config = {})          // -> context_handle         [stub]
```

## command_list — records GPU work  (abstract)

```cpp
#include <shaped-graphics/command_list.hh>
// abstract; a backend subclasses it (protected ctor). obtained via ctx.create_command_list()
// recording API (copy/upload/download buffer, ...) — pure-virtual — lands with the first milestone
```

## buffer — GPU-resident, immutable shape  (abstract)

```cpp
#include <shaped-graphics/buffer.hh>
// abstract; a backend subclasses it. protected ctor: buffer(size_in_bytes, usage)
b.size_in_bytes()                  // isize   (inline, cheap — no virtual call)
b.usage()                          // sg::buffer_usage
// shape metadata (_size_in_bytes/_usage) is protected in the base; backend buffers inherit it
```

## backends — subclass the abstract sg types

```cpp
// context/command_list/buffer are abstract; backends derive directly (no separate bridge layer):
sg::backend::dx12::dx12_context   : sg::context        // + dx12_command_list, dx12_buffer
sg::backend::vulkan::vulkan_context : sg::context      // + vulkan_command_list, vulkan_buffer
// creation: sg::create_dx12_context / sg::create_vulkan_context  (declared in the backend headers)
// escape hatch: dynamic_cast<sg::backend::vulkan::vulkan_context*>(ctx.get()) — "here be dragons"
```
