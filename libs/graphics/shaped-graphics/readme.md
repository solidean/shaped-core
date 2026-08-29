# shaped-graphics

The graphics-API wrapper.
Namespace `sg`, depending on **clean-core** (vocabulary types and assertions) and **typed-geometry** (math types).
Part of the [graphics family](../../../docs/graphics.md) — `sv → sr → sg → tg/cc`.

sg exposes a small, backend-agnostic surface — `context`, `command_list`, and the GPU resource types — over concrete graphics backends.
dx12 and vulkan are tier 1; metal and webgpu tier 2; opengl and webgl legacy.
Only the two tier-1 backends exist — nothing is written for the others yet.

The library is at an **early stage**, though not a stub: the sg core and the **dx12** backend are real across transfer, barriers, bindings, pipelines, raytracing, queries and presentation.
**vulkan** is real through the device, resources, memory heaps, inline transfer and copies, the whole bind path, compute dispatch and raster.
Ray tracing, presentation and both async transfer scopes are still stubs there.
[docs/structure.md](docs/structure.md) is the per-module `[done]` / `[in progress]` / `[planned]` roadmap.

## Design at a glance

- **Handles.** Every shared type `xyz` has an `xyz_handle`, almost always `std::shared_ptr<sg::xyz const>` — `raw_buffer_handle`, `blas_handle`, the pipelines and layouts.
  `context_handle` and `swapchain_handle` are the mutable-driver exceptions, without the `const`.
  Public factories return the handle.
  A command list is the exception: it is a move-only `std::unique_ptr<command_list>` with no handle typedef.
- **Mutable drivers vs shared-immutable resources.** `context` and `command_list` are mutable, stateful and single-threaded.
  `raw_buffer` and `raw_texture` are immutable in *shape* and act like a span over mutable GPU-resident memory.
  A texture also has a typed `texture<Traits>` wrapper, with concept-gated getters, over the raw resource.
- **No host-visible resources.** There are no CPU-mapped buffers or textures; host↔device transfer is a globally shared resource sg manages, driven through command lists.
- **Abstract interfaces, backends derive directly.** A backend subclasses `context` / `command_list` / `raw_buffer` / `raw_texture` directly, with no separate bridge or impl layer.
  Cheap shared metadata — a buffer's size and usage — lives in the base as protected members with non-virtual accessors, so reading it costs no virtual call.
- **sg does not depend on the backends.** The arrow points one way, backends → sg, so there is no `sg::create_context` in the core.
  Each backend library exposes an `sg::create_<backend>_context(config)` factory with its own config type, and `backend_kind` is a coarse tag rather than a backend identity.
- **Backends are separate static libraries**, smurf-named and namespaced (`sg::backend::dx12::dx12_context`), one per API under [backends/](backends/).
  They are public and readability-first, and each is built only where its platform allows.

The rules and the reasoning behind each: [docs/coding-guidelines.md](docs/coding-guidelines.md).

## File organization

Source lives in `src/shaped-graphics/`, grouped by topic.
Headers are included by their full path from `src/`, e.g. `#include <shaped-graphics/resource/texture.hh>`.

| Path                | What's in it |
|---------------------|--------------|
| (root)              | the cross-cutting vocabulary only: `fwd.hh` (fwd decls + `*_handle` typedefs), `all.hh`, `types.hh`, `exceptions.hh`, `bytes_future.hh` |
| `barrier/`          | the access-tracking substrate: access/stage/layout vocabulary, the three-timeline `resource_access_state`, the subresource partition, the command-list slot model |
| `binding/`          | `compiled_shader`, the reflected `binding` vocabulary, `sampler`, and the bind path's `binding_group_layout` / `pipeline_layout` / `binding_group` |
| `command_list/`     | the abstract `command_list` and its seven recording scopes (`upload`, `download`, `copy`, `compute`, `raster`, `raytracing`, `query`) |
| `context/`          | the abstract `context`, its six lifetime/transfer scopes (`persistent`, `transient`, `upload`, `download`, `uncached`, `cached`), and `pipeline_cache` |
| `memory/`           | `allocation_info` + `memory_heap` — placed vs dedicated backing memory |
| `compute/`          | `compute_pipeline` |
| `raster/`           | `raster_pipeline` and the fixed-function state a graphics PSO aggregates: topology, rasterization, blend, depth-stencil, vertex input |
| `present/`          | `swapchain` and the presentation path |
| `query/`            | GPU queries — `gpu_timestamp` today |
| `raytracing/`       | `blas`/`tlas`, `raytracing_pipeline`, `raytracing_shader_table` |
| `resource/`         | the GPU resource surface: `raw_buffer`/`buffer<T>`, `raw_texture`/`texture<Traits>`, `views.hh`, `pixel_format`, subresource addressing |
| `routine/`          | the render-routine framework: `render_routine<Derived>`, `routine_registry`, `reload_generation` |
| `backends/<api>/`   | concrete per-backend static libraries (`dx12/`, `vulkan/`) that subclass the abstract types, each smurf-named in `sg::backend::<api>` |

Where a folder is named after a type, that type's header repeats the name (`command_list/command_list.hh`, `context/context.hh`) and doubles as the folder's umbrella.
`impl/` subfolders (e.g. `resource/impl/`) are internal — they are `sg::impl` and deliberately outside the public header set.

## Building & testing

Build and test through the repo driver — never run the `shaped-graphics-test` binary directly:

```bash
uv run dev.py test "sg "     # just the shaped-graphics tests while iterating
uv run dev.py build          # the whole repo, incl. platform-enabled backends
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/shaders.md](docs/shaders.md) — how the shader system works end to end: declaring a package, `acquire(ctx)`, hot reload, shipping.
  Most of it lives *downstream* of sg, in shaped-shader-library and shaped-shader-compiler-dxc, but that is where to start looking.
- [docs/_index.md](docs/_index.md) — shaped-graphics' documentation hub.
- [docs/structure.md](docs/structure.md) — the module roadmap and status.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — the sg-specific conventions on top of the repo-wide ones.
  Handles, backend smurf naming, duplication-over-abstraction.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
