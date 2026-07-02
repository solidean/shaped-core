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
  context.hh/.cc                  [in progress] abstract; pure-virtual create_command_list/create_buffer
  command_list.hh/.cc             [in progress] abstract; recording API planned
  buffer.hh/.cc                   [in progress] abstract; protected shape (size/usage) done
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

Fallible creates (`create_*_context`, `create_command_list`, `create_buffer`) return `cc::result`;
programmer misuse (e.g. `size <= 0`) asserts rather than returning an error. See the
[coding-guidelines](coding-guidelines.md) and the repo error-handling policy.

## Context creation & backend decoupling

sg never depends on a backend (the arrow is backends → sg). There is no `sg::create_context` in
the core; each backend library exposes an `sg::create_<backend>_context(config)` factory in the
`sg` namespace with its own config type. `backend_kind` is a coarse, non-exhaustive tag (for
interpreting escape-hatch handles), not a backend identity — so a debug/cpu/remote backend drops
in without the core knowing it. See the [coding-guidelines](coding-guidelines.md).

## Resource & transfer model

Resources (`buffer`, planned `texture`) are **shared-immutable**: fixed shape, span-like over
mutable GPU memory, held via `*_handle`. A resource may be **empty** (size 0 — allocates no GPU
storage). There are **no host-visible resources**; host↔device transfer is a globally shared
resource sg manages, driven through command lists. See the [coding-guidelines](coding-guidelines.md).

## Ownership & lifetime

- **Resources are shared** (`buffer_handle` = `shared_ptr`); **command lists are move-only**
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

`memory_heap` + `allocation_info` are the placement model: a resource's backing memory either is
**dedicated** (self-allocating) or is **placed** into a shared `memory_heap`. A `memory_heap` is a
factory for `allocation_info` — the caller queries `memory_requirements` (backend-reported alignment +
actual occupied size, which may exceed the requested size), its own allocator picks an offset, and the
heap alignment/bounds-checks it and mints an `allocation_info` pointing back into itself (an
`enable_shared_from_this` handle keeps the heap alive as long as any placement references it). Reporting
the occupied size from the backend is what lets textures — whose real footprint the driver decides —
share this path. `allocation_info` is a cheap value type: a nullable
`memory_heap_handle` (null = dedicated), an offset/size, and an `allocation_scope` lifetime hint
(`persistent`/`transient`; transient is only a hint — the backend still tracks in-flight GPU usage).

Intended flow: query requirements → allocator picks an offset → `heap.acquire_allocation_for_*(...)` →
pass the `allocation_info` to the matching create_*. **Not yet wired:** `context` has no
`create_memory_heap`, and `create_buffer` does not yet take an `allocation_info` — both are the natural
next step once this stub grows. The per-resource-kind fan-out (`acquire_allocation_for_buffer`, planned
`_texture`) mirrors sg's per-kind create methods.

## Planned surface (beyond the current stubs)

```text
buffer transfer      [planned]  command_list upload / download / copy  (first milestone)
texture              [planned]  GPU-resident images + views
pipeline             [planned]  graphics/compute pipelines + shader modules
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
2. command_list buffer upload / download / copy            [planned]      first real milestone
3. real dx12 + vulkan backends for (2) (+ SDK detection)   [planned]
4. textures + views                                        [planned]
5. pipelines + shaders                                     [planned]
6. presentation (swapchain/surface) + submission/sync      [planned]
7. tier 2 backends (metal, webgpu)                         [planned]
8. legacy backends (opengl, webgl)                         [planned]
```
