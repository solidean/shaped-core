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

- [shaders](shaders.md) — how a shader gets from a file you edit to something a context can build a
  pipeline from: declaring a package, `acquire(ctx)`, hot reload, and dev-vs-shipping. Most of that
  machinery lives *downstream* of sg (shaped-shader-library, shaped-shader-compiler-dxc) — sg owns only
  `compiled_shader` and what a context accepts — but this is where to start looking.
- [render-routines](render-routines.md) — the render-routine framework: `sg::render_routine<Derived>`
  (3-phase, hot-reload-aware init, reached by type via `acquire(cmd)` / `prewarm(ctx)` / `evict(ctx)`),
  the per-context `ctx.routines` registry (lazy self-registration, `clear()`), and the
  `sg::reload_generation` counter.
  Concrete routines live in shaped-rendering.
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
- [barriers](concepts/barriers.md) — access tracking + GPU barriers: access inferred from each op, the
  three-timeline minimal-barrier state machine, and the concurrent-command-list slot model.
- [views](concepts/views.md) — strongly-typed resource views: typed by element type `T`, the
  access×layout axes shared across shading languages, and the erased `raw_view` backends consume.
- [textures](concepts/textures.md) — the `raw_texture` resource vs the typed `texture<Traits>` wrapper,
  the derived-not-flagged `texture_description` shape, and the restrictive `pixel_format` set.
- [bindings](concepts/bindings.md) — `compiled_shader` reflection: the backend-agnostic `binding`
  vocabulary (`binding_type`, `(set, index)`) and how a binding validates a bound `raw_view`.
- [caches](concepts/caches.md) — the content-addressed get-or-create caches for binding layouts,
  compute pipelines (async), and compiled shaders: `ctx.uncached` (raw) vs `ctx.cached`, keys, and tiers.
- [acceleration structures](concepts/acceleration-structures.md) — ray-tracing `blas`/`tlas`: opaque
  driver-built structures whose creation is a recorded `cmd.raytracing` build, the DXR∩Vulkan input
  vocabulary, and the persistent-vs-transient handle lifetime.
- [raytracing pipeline](concepts/raytracing-pipeline.md) — the full DXR path: a `raytracing_pipeline`
  (state object) + `raytracing_shader_table` + `cmd.raytracing.dispatch_rays`, the two-phase handle/index
  model, and why records hold only a shader identifier.
- [raster pipeline](concepts/raster-pipeline.md) — the graphics path: a `raster_pipeline` (PSO) with its
  fixed-function state baked in, why the target formats live in the description, and why draws sit on
  `cmd.raster` / `cmd.raster.manual` rather than the `rendering_scope` handle.
- [inline upload](concepts/upload.inline.md) — latency-critical CPU→GPU buffer writes through an
  epoch-reclaimed UPLOAD ring buffer, usable later in the same command list.
- [async upload](concepts/upload.async.md) — bulk CPU→GPU streaming on a dedicated copy queue
  (`ctx.upload`), off the frame path, with automatic per-resource sync so later lists auto-wait.
- [inline download](concepts/download.inline.md) — asynchronous GPU→CPU readback through a READBACK
  ring buffer drained by an actor, with epoch-granular space reclaim and drop-to-cancel futures.
- [async download](concepts/download.async.md) — bulk GPU→CPU readback on a dedicated copy queue
  (`ctx.download`), off the frame path, with automatic per-resource sync in both directions.
- [GPU queries](concepts/queries.md) — `cmd.query.record_gpu_timestamp`: pooled query heaps leased per
  list, one batched readback per heap at submit, and the poll-after-submit `gpu_timestamp` result.

## Conventions

- Namespace `sg`; depends on clean-core (vocabulary types + assertions) and typed-geometry
  (math types).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  sg-specific [coding-guidelines](coding-guidelines.md). `.clang-format` is authoritative for
  formatting.
