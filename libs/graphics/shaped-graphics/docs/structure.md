# shaped-graphics structure (sg::)

The living roadmap for shaped-graphics. Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started
- **[stub]** — type/shape exists, body is `CC_UNREACHABLE("not implemented yet")`

Update the tags as the API lands. This document is design intent, not a guarantee of final API.

## Goals

- A small, backend-agnostic graphics-API surface (`context`, `command_list`, GPU resources).
- Concrete backends as independent static libraries; validation shared in the sg core via the
  backend bridge, backend-specific work duplicated rather than abstracted.
- Shared-immutable resources fronting GPU-resident memory; all host↔device transfer managed by
  sg (no host-visible resources exposed).

## Top-level structure

```text
src/shaped-graphics/
  fwd.hh / all.hh / types.hh      [in progress]
  context.hh/.cc                  [stub]        mutable driver / resource factory
  command_list.hh/.cc             [stub]        records GPU work
  buffer.hh/.cc                   [in progress] shape done; transfer/creation stubbed
  backend/                        [in progress] pure-virtual bridge
    backend_context.hh            [in progress] kind() only; resource/command creation planned
    backend_command_list.hh       [in progress] recording contract planned
    backend_buffer.hh             [in progress] GPU resource an sg::buffer fronts
backends/
  dx12/                           [stub]        sg::backend::dx12 + sg::create_dx12_context (Windows)
  vulkan/                         [stub]        sg::backend::vulkan + sg::create_vulkan_context (native desktop)
  metal/                          [planned]     tier 2
  webgpu/                         [planned]     tier 2
  opengl/                         [planned]     legacy compat
  webgl/                          [planned]     legacy compat
```

## Backend tiers

- **Tier 1 (now):** dx12, vulkan. Both stubbed today; the first real implementation targets.
- **Tier 2 (soon):** metal, webgpu.
- **Legacy compat (planned):** opengl, webgl.

A backend is built only where it is available for the platform/build. Today the gates are
platform-only and the stubs carry no SDK dependency (dx12 → Windows, vulkan → native desktop);
real `find_package`/SDK detection lands with the first backend implementation.

## Context creation & backend decoupling

sg never depends on a backend (the arrow is backends → sg). There is no `sg::create_context` in
the core; each backend library exposes an `sg::create_<backend>_context(config)` factory in the
`sg` namespace with its own config type. `backend_kind` is a coarse, non-exhaustive tag (for
interpreting escape-hatch handles), not a backend identity — so a debug/cpu/remote backend drops
in without the core knowing it. See the [coding-guidelines](coding-guidelines.md).

## Resource & transfer model

Resources (`buffer`, planned `texture`) are **shared-immutable**: fixed shape, span-like over
mutable GPU memory, held via `*_handle`. There are **no host-visible resources**; host↔device
transfer is a globally shared resource sg manages, driven through command lists. See the
[coding-guidelines](coding-guidelines.md).

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
