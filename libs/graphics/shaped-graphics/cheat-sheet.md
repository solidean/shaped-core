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

## command_list — records GPU work  (stubbed)

```cpp
#include <shaped-graphics/command_list.hh>
sg::command_list(std::shared_ptr<sg::backend_command_list>)  // wraps a backend command list
// recording API (copy/upload/download buffer, ...) lands with the first milestone
```

## buffer — GPU-resident, immutable shape

```cpp
#include <shaped-graphics/buffer.hh>
sg::buffer(size_in_bytes, usage, shared_ptr<backend_buffer>)  // shape inline; backend owns the resource
b.size_in_bytes()                  // isize   (inline, cheap — no virtual call)
b.usage()                          // sg::buffer_usage
```

## backend bridge (interfaces)

```cpp
#include <shaped-graphics/backend/backend_context.hh>
#include <shaped-graphics/backend/backend_command_list.hh>
#include <shaped-graphics/backend/backend_buffer.hh>
sg::backend_context        // pure-virtual; kind() = 0; one concrete impl per backend
sg::backend_command_list   // pure-virtual; recording contract per backend
sg::backend_buffer         // pure-virtual; the GPU resource an sg::buffer fronts
// concrete impls: sg::backend::dx12::dx12_context / ::dx12_command_list  (+ vulkan)
// creation: sg::create_dx12_context / sg::create_vulkan_context (in the backend headers)
// escape hatch: dynamic_cast a bridge object back to its sg::backend::<api>::* type — "here be dragons"
```
