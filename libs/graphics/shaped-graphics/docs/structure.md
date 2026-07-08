# shaped-graphics structure (sg::)

The living roadmap for shaped-graphics. Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started
- **[stub]** — type/shape exists, body is `CC_UNREACHABLE("not implemented yet")`

Update the tags as the API lands. This document is design intent, not a guarantee of final API.

## Goals

- A small, backend-agnostic graphics-API surface (`context`, `command_list`, GPU resources).
- The public `context` / `command_list` / `buffer` are abstract interfaces; concrete backends are
  independent static libraries that subclass them directly (no separate bridge layer), with
  backend-specific work duplicated rather than abstracted.
- Shared-immutable resources fronting GPU-resident memory; all host↔device transfer managed by
  sg (no host-visible resources exposed).

## Top-level structure

```text
src/shaped-graphics/
  fwd.hh / all.hh / types.hh      [in progress]
  context.hh/.cc                  [in progress] abstract; infallible create_command_list over pure-virtual try_create_*; sticky device-loss status; creates funneled via ctx.persistent
  exceptions.hh                   [in progress] typed sg exceptions (device_lost / allocation / pipeline_creation / binding_group)
  context.persistent.hh/.cc       [in progress] context_persistent_scope: ctx.persistent resource factory (back-ref + friend of context)
  command_list.hh/.cc             [in progress] abstract; recording API planned
  raw_buffer.hh/.cc               [in progress] abstract; protected shape (size/usage) done; as_* view factories
  pixel_format.hh                 [in progress] restrictive texel-format enum + helpers (depth/compressed/block-size)
  raw_texture.hh/.cc              [in progress] abstract; texture_description + protected shape; creation only (no views/barriers yet)
  texture.hh                      [in progress] texture<Traits> typed wrapper (concept-gated getters) + shape typedefs
  views.hh                        [in progress] strongly-typed buffer views (uniform/readonly/readwrite<T>, byte=raw)
                                                + erased raw_view; texture/texel views deferred (resource + format exist; binding is future)
  binding.hh                      [in progress] backend-agnostic reflection: binding + binding_type ((set,index); maps to view)
  compiled_shader.hh              [in progress] shader data model: bytecode blob + stage/format/entry + reflected bindings
  binding_layout.hh/.cc           [in progress] abstract: the bindable-set schema (built from bindings); dx12 = root sig (vulkan stub)
  compute_pipeline.hh/.cc         [in progress] abstract: compute shader + layout; dx12 = PSO (vulkan stub)
  binding_group.hh/.cc            [in progress] abstract: layout instance bound to raw_views (named_view); dx12 = heap range + views (vulkan stub)
  command_list.compute.hh/.cc     [in progress] cmd.compute scope: bind_pipeline / bind_group / dispatch (dx12 real; vulkan stub)
  allocation_info.hh              [stub]        value type: placement handle (heap/offset/size + scope); null heap = dedicated
  memory_heap.hh/.cc              [stub]        abstract; memory_requirements struct + alloc-info factory (query/acquire per kind); backend requirements hook pure-virtual
backends/                                       # each subclasses the abstract sg types directly
  dx12/                           [in progress] sg::backend::dx12 + sg::create_dx12_context (Windows): real device/cmd-list/buffer
    tests/                                      own *-test binary for dx12-specific tests (WARP + hardware)
  vulkan/                         [in progress] sg::backend::vulkan + sg::create_vulkan_context (native desktop): real device/cmd-list/buffer
  metal/                          [planned]     tier 2
  webgpu/                         [planned]     tier 2
  opengl/                         [planned]     legacy compat
  webgl/                          [planned]     legacy compat
```

## Backend tiers

- **Tier 1 (now):** dx12, vulkan. Both are real (device + command list + GPU buffer + epoch system).
- **Tier 2 (soon):** metal, webgpu.
- **Legacy compat (planned):** opengl, webgl.

A backend is built only where it is available for the platform/build. The gates are platform-only
(dx12 → Windows, vulkan → native desktop). dx12 links the Windows-SDK D3D12 libs
(`d3d12 dxgi dxguid`), always present on the Windows path; vulkan gates on `find_package(Vulkan)` and
links `Vulkan::Vulkan` (the loader + headers), so it builds wherever a Vulkan SDK is installed.

Error handling follows the repo policy in [docs/error-handling.md](../../../../docs/error-handling.md):
resource creates offer a throwing default (`create_raw_buffer` → returns the handle, raises a typed
`sg::exception` — see `exceptions.hh`) plus a fallible `try_create_*` returning `cc::result` (the only
thing backends implement). `create_command_list()` is infallible (returns the handle; throws only on
device loss); `create_<backend>_context` still returns `cc::result` (environment failure). Programmer
misuse (e.g. `size < 0`, missing usage, using a transient resource past its epoch) asserts rather than
returning an error. Device loss is a sticky, global status (`ctx.is_device_lost()`) surfaced by a throw
at submit / advance / fence waits — deliberately kept off the `try_*` channel. See the
[coding-guidelines](coding-guidelines.md).

## Context creation & backend decoupling

sg never depends on a backend (the arrow is backends → sg). There is no `sg::create_context` in
the core; each backend library exposes an `sg::create_<backend>_context(config)` factory in the
`sg` namespace with its own config type. `backend_kind` is a coarse, non-exhaustive tag (for
interpreting escape-hatch handles), not a backend identity — so a debug/cpu/remote backend drops
in without the core knowing it. See the [coding-guidelines](coding-guidelines.md).

## Resource & transfer model

Resources (`raw_buffer`, `raw_texture`) are **shared-immutable**: fixed shape, span-like over
mutable GPU memory, held via `*_handle`. A resource may be **empty** (size 0 — allocates no GPU
storage). There are **no host-visible resources**; host↔device transfer is a globally shared
resource sg manages, driven through command lists. See the [coding-guidelines](coding-guidelines.md).

Resource creation is reached through a **lifetime scope** on the context rather than the context
directly: `ctx.persistent.create_raw_buffer(...)`. A scope (`sg::context_persistent_scope`) is a thin facade with a
back-reference to its context; the actual `create_*` virtual stays on `context` (backends implement it)
and the scope — a friend — funnels through it, tagging the request with its lifetime. Today only the
persistent scope exists; a transient scope (per-frame/epoch resources, mapping onto `lifetime_scope`)
is the planned second one.

## Ownership & lifetime

- **Resources are shared** (`raw_buffer_handle` = `shared_ptr`); **command lists are move-only**
  (`std::unique_ptr<command_list>`, no handle typedef) — record once, submit once, passed by
  reference.
- **Backend-typed create methods** (`create_dx12_buffer` → `dx12_buffer_handle`, …) are the real
  implementations; the abstract `sg::context` virtuals are thin forwarders. Prefer the backend-typed
  method when you hold a concrete backend context — no downcasts.
- A **context must outlive** every object it creates. Objects hold a literal backref to it.
  `submit`/`drop` **consume** the command list (moved in), so submit-once / drop-once is structural;
  either that or scope exit destroys the list, and the destructor is the single teardown point. A
  context is **shut down before destruction** (virtual `shutdown()`, auto-run by the backend dtor).

See the [coding-guidelines](coding-guidelines.md) for the rationale on each.

## Memory placement

Every resource's backing memory is either **dedicated** (self-allocating) or **placed** into a shared
`memory_heap`; an `allocation_info` (passed to each create_*) names which. `memory_heap` is an immutable
factory for `allocation_info` — allocation tracking lives in the caller's own allocator on top. See
[concepts/memory.md](concepts/memory.md) for the lifetime modes (persistent vs transient) and the
placed-vs-dedicated system.

Only the **dedicated** case is implemented today; a placed allocation asserts in the backends
(`is_dedicated()` gate) until placement lands. **Not yet wired:** `context` has no `create_memory_heap`,
and neither backend can bind a resource into a heap yet.

## Planned surface (beyond the current stubs)

```text
buffer transfer      [in progress]  command_list inline upload / download / copy (dx12 real, vulkan stub)
barriers             [in progress]  inferred access + state tracking + concurrent-list slot model; dx12 enhanced
                                  barriers real for buffers + textures (subresource-range layout transitions,
                                  entry-layout revert); no public texture op drives it yet, vulkan pending
views                [in progress]  strongly-typed resource views; buffer views done, binding path + texture/texel deferred
bindings             [in progress]  compiled_shader + binding vocab; binding_layout/group + compute_pipeline (dx12 real, vulkan stub)
texture              [in progress]  raw_texture + texture<Traits> + pixel_format; creation done (dx12 real, vulkan minimal);
                                  views / layout barriers / copies remain
pipeline             [in progress]  compute pipeline + bind path (dx12 real, vulkan stub); graphics pipelines + shader compiler planned
sampler              [planned]
swapchain / surface  [planned]  presentation
epochs / submission  [in progress]  epoch counter + direct-queue epoch/submission timelines, advance/retire,
                                  deferred deletion + finalizers, allocator/pool recycling (dx12 + vulkan real)
```

The **epoch system** is the frame-level GPU-lifetime + CPU↔GPU sync mechanism: only the concept
(`sg::epoch` / `sg::submission_token` and the `sg::context` contract) is shared; each backend
realizes it (dx12 does; a backend may also uphold the contract without real in-flight tracking). It
underpins safe resource reclamation and command-allocator recycling. See
[concepts/epochs.md](concepts/epochs.md).

## Initial implementation order

```text
1. core types + backend bridge stubs + dx12/vulkan stubs   [in progress]  (this bootstrap)
2. command_list buffer inline upload / download            [in progress]  dx12 real; copy + vulkan pending
3. real dx12 + vulkan backends for (2) (+ SDK detection)   [in progress]  dx12 done; vulkan is a TODO stub
4. textures + views                                        [in progress]  texture resource + creation done (dx12 real, vulkan minimal); texture/texel views + binding path remain
5. pipelines + shaders                                     [in progress]  compute bind path dx12-real (vulkan + graphics + shader compiler pending)
6. presentation (swapchain/surface) + submission/sync      [planned]
7. tier 2 backends (metal, webgpu)                         [planned]
8. legacy backends (opengl, webgl)                         [planned]
```
