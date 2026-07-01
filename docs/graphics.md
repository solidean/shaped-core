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
- **The backend bridge.** `sg::backend_context` / `sg::backend_command_list` /
  `sg::backend_buffer` (under
  [src/shaped-graphics/backend/](../libs/graphics/shaped-graphics/src/shaped-graphics/backend/))
  are pure-virtual interfaces. `sg::context` holds a `std::shared_ptr` to one and runs
  sg-generic validation before delegating — so validation is written once and every backend
  inherits it.
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

## Building & testing

All three build and test through the repo driver like every other library:

```bash
uv run dev.py test "sg "     # just the shaped-graphics tests (also "sr ", "sv ")
uv run dev.py build          # the whole repo, incl. platform-enabled backends
```

See [guides/building-and-testing.md](guides/building-and-testing.md) for the full workflow
and [libraries.md](libraries.md) for the full library catalog and dependency graph.
