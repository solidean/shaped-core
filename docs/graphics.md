# The graphics library family (sg / sr / sv)

`libs/graphics/` is shaped-core's graphics stack: three libraries layered on top of the
`base` foundation, each building on the one below.

```text
shaped-viewer     (sv::)   professional, RTX-enabled visualization renderer
     ↓
shaped-rendering  (sr::)   render routines & helpers on top of sg
     ↓
shaped-graphics   (sg::)   graphics-API wrapper (context / command_list / resources / backends)
     ↓
typed-geometry (tg::) + clean-core (cc::)
```

This is an **early-stage** family. Today it is docs + buildable skeletons: sg has stubbed
core types and dx12/vulkan backend stubs; sr and sv are empty-but-buildable. The first real
milestone is command-list buffer **upload / download / copy** in sg.

## The libraries

### shaped-graphics — `sg::`

The graphics-API wrapper. It exposes a small, backend-agnostic surface — `context`,
`command_list`, and GPU resource types (`buffer` today; `texture`, `pipeline`, … to come) —
over concrete graphics backends.

- **Backends are separate static libraries** under
  [shaped-graphics/backends/](../libs/graphics/shaped-graphics/backends/), one per API. dx12
  and vulkan are **tier 1** (both stubbed today); metal and webgpu are **tier 2** (soon);
  opengl and webgl are **legacy compat** (planned). A backend is built only where it is
  available for the platform/build.
- **Abstract interfaces, backends derive directly.** `sg::context` / `sg::command_list` /
  `sg::buffer` (under
  [src/shaped-graphics/](../libs/graphics/shaped-graphics/src/shaped-graphics/)) are abstract; a
  backend subclasses them directly (`sg::backend::vulkan::vulkan_context : sg::context`) — there is
  no separate bridge/impl layer. Cheap shared metadata (a buffer's size/usage) lives in the base
  as protected members, so reading it costs no virtual call and every backend inherits it.
- **sg does not depend on the backends.** The dependency arrow points one way (backends → sg):
  there is no `sg::create_context` in the core. Each backend library instead exposes an
  `sg::create_<backend>_context(config)` factory with its own config type. `backend_kind` is a
  coarse tag, not a backend identity — a debug/cpu/remote backend is just as valid as dx12/vulkan.
  This decoupling is intentional, so sg never overfits to today's GPU backends.
- **Resource model.** `context` and `command_list` are the mutable drivers. Resources
  (`buffer`, later `texture`) are **shared-immutable**: their shape is fixed and they behave
  like a span over mutable GPU-resident memory. There are **no host-visible buffers or
  textures** in the API — PCIe transfer is a globally shared resource that sg manages.
- **Handles.** Every shared type has an `xyz_handle` typedef =
  `std::shared_ptr<sg::xyz>` (e.g. `sg::context_handle`).

See the [shaped-graphics readme](../libs/graphics/shaped-graphics/readme.md) and its
[coding guidelines](../libs/graphics/shaped-graphics/docs/coding-guidelines.md) for the
load-bearing conventions (handles, backend smurf naming, the duplication-over-abstraction
stance).

### shaped-rendering — `sr::`

Render routines and helpers built on sg: the common building blocks of a renderer — mipmap
generation, texture compression, tonemapping, and similar. Early-stage skeleton today. See
the [shaped-rendering readme](../libs/graphics/shaped-rendering/readme.md).

### shaped-viewer — `sv::`

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly
API, serving Shaped Code's visualization needs. Built on sr. Early-stage skeleton today. See
the [shaped-viewer readme](../libs/graphics/shaped-viewer/readme.md).

### shaped-shader-compiler-dxc — `ssc::dxc::`

A side utility (not part of the sv→sr→sg chain): a lean wrapper over the DirectX Shader Compiler
(DXC) that turns HLSL into an `sg::compiled_shader` — bytecode + reflected bindings + compute
workgroup size — filling the "compilation is not part of sg yet" gap noted in
[compiled_shader.hh](../libs/graphics/shaped-graphics/src/shaped-graphics/compiled_shader.hh). It
depends only on **shaped-graphics**. Two-step API: `preprocess` (resolve `#include`s via a
caller-supplied resolver) then `compile` (already-flattened source → DXIL + reflection).

- **Windows-only** today: links DXC and uses the Windows SDK's `d3d12shader.h` reflection.
- **DXC is downloaded on demand**, not vendored or built from source: the first Windows configure runs
  [`extern/dxc/download-dxc.py`](../extern/dxc/download-dxc.py), which fetches the pinned official
  release (`v1.9.2602.24`, SHA-256 verified) for the host arch into `extern/dxc/.install/` (a
  few-second download; `SC_SKIP_DXC=1` skips it). The release ships `dxil.dll`, so emitted DXIL is
  signed (runs on dx12 without developer mode), and includes `arm64` binaries.

See the [shaped-shader-compiler-dxc readme](../libs/graphics/shaped-shader-compiler-dxc/readme.md).

## Building & testing

All three build and test through the repo driver like every other library:

```bash
uv run dev.py test "sg "     # just the shaped-graphics tests (also "sr ", "sv ")
uv run dev.py build          # the whole repo, incl. platform-enabled backends
```

See [guides/building-and-testing.md](guides/building-and-testing.md) for the full workflow
and [libraries.md](libraries.md) for the full library catalog and dependency graph.
