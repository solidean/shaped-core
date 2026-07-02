# shaped-graphics docs

Documentation hub for shaped-graphics. For the library overview, types, and how to include
headers, start at the [readme](../readme.md). For the whole graphics family see
[docs/graphics.md](../../../../docs/graphics.md); for repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

## Source organization

shaped-graphics' headers live in `src/shaped-graphics/`:

```text
shaped-graphics/
  fwd.hh          # fwd decls + *_handle typedefs
  all.hh          # umbrella
  types.hh        # backend_kind, buffer_usage
  context.hh      # abstract driver / factory
  command_list.hh # abstract; records GPU work
  buffer.hh       # abstract; GPU-resident, immutable shape (protected metadata)
backends/         # concrete per-backend static libs (dx12/, vulkan/) that subclass the sg types
```

## Topics

- [structure](structure.md) — the module roadmap with `[done]`/`[in progress]`/`[planned]`
  status. This is the living design document; update it as the API lands.
- [coding-guidelines](coding-guidelines.md) — sg-specific conventions on top of the repo-wide
  ones: handles, mutable-vs-immutable types, the backend bridge, backend smurf naming, and the
  duplication-over-abstraction stance. Extend it whenever generic advice turns out not to fit sg.
- [TODO](TODO.md) — running list of known follow-ups.

## Conventions

- Namespace `sg`; depends on clean-core (vocabulary types + assertions) and typed-geometry
  (math types).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  sg-specific [coding-guidelines](coding-guidelines.md). `.clang-format` is authoritative for
  formatting.
