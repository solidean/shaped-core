# shaped-graphics

The graphics-API wrapper. Namespace `sg`. Depends on **clean-core** (vocabulary types +
assertions) and **typed-geometry** (math types). Part of the
[graphics family](../../../docs/graphics.md) (`sv → sr → sg → tg/cc`).

sg exposes a small, backend-agnostic surface — `context`, `command_list`, and GPU resource
types — over concrete graphics backends (dx12/vulkan tier 1; metal/webgpu tier 2; opengl/webgl
legacy).

This library is at an **early stage**: the core types and the dx12/vulkan backends are
**stubs** (`CC_UNREACHABLE("not implemented yet")`). The first milestone is command-list buffer
upload/download/copy. See [docs/structure.md](docs/structure.md) for what is `[done]` vs
`[planned]`.

## Design at a glance

- **Handles.** Every shared type `xyz` has `xyz_handle = std::shared_ptr<sg::xyz>`
  (`context_handle`, `command_list_handle`, `raw_buffer_handle`). Public factories return handles.
- **Mutable drivers vs shared-immutable resources.** `context` and `command_list` are mutable,
  stateful, single-threaded. `raw_buffer` and `raw_texture` are immutable in *shape* and act
  like a span over mutable GPU-resident memory. A texture also has a typed `texture<Traits>`
  wrapper (concept-gated getters) over the raw resource.
- **No host-visible resources.** There are no CPU-mapped buffers/textures; host↔device transfer
  is a globally shared resource sg manages, driven through command lists.
- **Abstract interfaces, backends derive directly.** `context`, `command_list`, `raw_buffer`, and `raw_texture` are
  abstract; a backend subclasses them directly (`sg::backend::vulkan::vulkan_context : sg::context`)
  — no separate bridge/impl layer. Cheap shared metadata (a buffer's size/usage) lives in the base
  as protected members with non-virtual accessors, so reading it costs no virtual call and every
  backend inherits it.
- **sg does not depend on the backends.** The dependency arrow points one way (backends → sg).
  There is no `sg::create_context` in the core; each backend library instead exposes an
  `sg::create_<backend>_context(config)` factory (e.g. `sg::create_vulkan_context`) with its own
  config type. `backend_kind` is a coarse tag, not a backend identity — a debug/cpu/remote
  backend is just as valid as dx12/vulkan.
- **Backends are separate static libraries**, smurf-named and namespaced
  (`sg::backend::dx12::dx12_context`), one per API under [backends/](backends/). They are public
  and readability-first (little encapsulation). A backend is built only where it is available for
  the platform/build.

## File organization

Source lives in `src/shaped-graphics/`:

| Path                | What's in it |
|---------------------|--------------|
| (root)              | `fwd.hh` (fwd decls + `*_handle` typedefs), `all.hh`, `types.hh`, `pixel_format.hh`, and the abstract `context` / `command_list` / `raw_buffer` / `raw_texture` (+ typed `texture.hh`) |
| `backends/<api>/`   | concrete per-backend static libraries (`dx12/`, `vulkan/`) that subclass the abstract types, each smurf-named in `sg::backend::<api>` |

## Building & testing

Build and test through the repo driver — never run the `shaped-graphics-test` binary directly:

```bash
uv run dev.py test "sg "     # just the shaped-graphics tests while iterating
uv run dev.py build          # the whole repo, incl. platform-enabled backends
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — shaped-graphics' documentation hub.
- [docs/structure.md](docs/structure.md) — the module roadmap and status.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — sg-specific conventions (handles,
  backend smurf naming, duplication-over-abstraction), on top of the repo-wide ones.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
