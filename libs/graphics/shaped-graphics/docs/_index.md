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
- [testing](testing.md) — the two test tiers: backend-agnostic API tests (`INVOCABLE_TEST`, run
  against every backend) vs per-backend smoke + internal-invariant suites, and where a new test goes.
- [TODO](TODO.md) — running list of known follow-ups.

## Concepts

Deep-dives on cross-cutting sg mechanisms — the load-bearing design decisions behind a topic.

- [backends](concepts/backends.md) — what a backend is, why we duplicate rather than abstract across
  them, and how each backend carries its own tests (feature smoke + backend-internal invariants).
- [epochs](concepts/epochs.md) — frame-level GPU resource lifetime + CPU↔GPU synchronization: the
  epoch counter/fence, advance/retire, deferred deletion, and finalizers.
- [threading](concepts/threading.md) — the per-backend `thread_model`: which context operations are
  concurrency-safe and which must be externally synchronized.
- [views](concepts/views.md) — strongly-typed resource views: typed by element type `T`, the
  access×layout axes shared across shading languages, and the erased `raw_view` backends consume.
- [bindings](concepts/bindings.md) — `compiled_shader` reflection: the backend-agnostic `binding`
  vocabulary (`binding_type`, `(set, index)`) and how a binding validates a bound `raw_view`.
- [inline upload](concepts/upload.inline.md) — latency-critical CPU→GPU buffer writes through an
  epoch-reclaimed UPLOAD ring buffer, usable later in the same command list.
- [async upload](concepts/upload.async.md) — bulk CPU→GPU streaming on a dedicated copy queue
  (`ctx.upload`), off the frame path, with automatic per-resource sync so later lists auto-wait.
- [inline download](concepts/download.inline.md) — asynchronous GPU→CPU readback through a READBACK
  ring buffer drained by an actor, with epoch-granular space reclaim and drop-to-cancel futures.

## Conventions

- Namespace `sg`; depends on clean-core (vocabulary types + assertions) and typed-geometry
  (math types).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  sg-specific [coding-guidelines](coding-guidelines.md). `.clang-format` is authoritative for
  formatting.
