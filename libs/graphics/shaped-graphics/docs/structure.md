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
backends/                                       # each subclasses the abstract sg types directly
  dx12/                           [in progress] sg::backend::dx12 + sg::create_dx12_context (Windows): real device/cmd-list/buffer
    tests/                                      own *-test binary for dx12-specific tests (WARP + hardware)
  vulkan/                         [stub]        sg::backend::vulkan + sg::create_vulkan_context (native desktop)
  metal/                          [planned]     tier 2
  webgpu/                         [planned]     tier 2
  opengl/                         [planned]     legacy compat
  webgl/                          [planned]     legacy compat
```

## Backend tiers

- **Tier 1 (now):** dx12, vulkan. dx12 is real (device + command list + GPU buffer); vulkan is
  still stubbed.
- **Tier 2 (soon):** metal, webgpu.
- **Legacy compat (planned):** opengl, webgl.

A backend is built only where it is available for the platform/build. The gates are platform-only
(dx12 → Windows, vulkan → native desktop). dx12 now links the Windows-SDK D3D12 libs
(`d3d12 dxgi dxguid`), always present on the Windows path; vulkan is still a stub with no SDK
dependency, and `find_package(Vulkan)`-style detection lands with its real implementation.

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

## Planned surface (beyond the current stubs)

```text
buffer transfer      [planned]  command_list upload / download / copy  (first milestone)
texture              [planned]  GPU-resident images + views
pipeline             [planned]  graphics/compute pipelines + shader modules
sampler              [planned]
swapchain / surface  [planned]  presentation
sync / submission    [planned]  fences, queues, submit
```

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
