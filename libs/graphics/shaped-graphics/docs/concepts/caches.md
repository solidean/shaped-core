# Concept: caches (layouts, pipelines, shaders)

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [bindings](bindings.md).

Building a `binding_layout` or a `compute_pipeline` is expensive and repetitive: the same shader is
compiled again and again, structurally identical layouts and PSOs are rebuilt, and a driver-level PSO
build takes multiple milliseconds. sg answers this with a **content-addressed, get-or-create cache** for
the pipeline schemas, and the shader compiler with a matching cache for compiled shaders. The verb is
**`acquire`** = "get it if it exists, otherwise create and store it".

## Three layers: raw, cached, and the async result

```
ctx.uncached.create_*   ── raw, always builds a fresh backend object (the escape hatch)
ctx.cached.acquire_*    ── get-or-create over the built-in pipeline_cache  ◀── prefer this
ssc::dxc::shader_cache  ── get-or-create over the DXC compiler (a separate lib)
```

- [`ctx.uncached`](../../src/shaped-graphics/context.uncached.hh) is the **raw layer**: it builds a fresh
  `binding_layout` / `compute_pipeline` every call. It sits apart from `ctx.persistent` / `ctx.transient`
  on purpose — a layout or pipeline is a *schema / PSO*, not a lifetime-scoped GPU resource, so it does
  not belong on a resource-lifetime scope. Uncached is a **deliberately poor default**: most code should
  not rebuild these per frame.
- [`ctx.cached`](../../src/shaped-graphics/context.cached.hh) is the **front door**. Every context owns a
  [`pipeline_cache`](../../src/shaped-graphics/pipeline_cache.hh) (with default in-memory tiers installed),
  reached here. `acquire_binding_layout` returns a shared handle; `acquire_compute_pipeline` returns an
  **async** handle whose build runs off-thread.
- The DXC [`shader_cache`](../../../shaped-shader-compiler-dxc/src/shaped-shader-compiler-dxc/shader_cache.hh)
  lives in the compiler library (not sg — sg has no compiler), and caches `compile()` the same way: same
  request → same async compiled shader, never recompiled.

`binding_group` is **not** here — it is a real per-instance descriptor allocation and stays on
`ctx.persistent` / `ctx.transient` (see [bindings](bindings.md)).

## The key is the content, not the handle

A cache entry is keyed by a [`cc::hash128`](../../../../base/clean-core/src/clean-core/common/hash128.hh)
computed from the **logical creation arguments**, so it is independent of any backend handle identity and
stable across runs. The arguments are serialized into a
[`cc::byte_stream_builder`](../../../../base/clean-core/src/clean-core/container/byte_stream_builder.hh) —
a length-prefixing byte blob builder — then hashed (XXH3-128). Length prefixes keep different splits of
the same data distinct, and sub-structs are hashed field by field (never a raw `memcpy` of a struct,
whose padding bytes would be nondeterministic).

- **binding layout** = the reflected `binding`s **plus the static samplers**. Static samplers are baked
  into the root signature, so a different static sampler is a different layout — it must be part of the key.
- **compute pipeline** = the shader's content (bytecode + entry point + compiler signature) combined with
  the **layout handle identity**. Pointer identity is enough because layouts are shared/persistent — which
  is exactly why you should acquire the layout *through the cache* first: two structurally identical
  layouts then collapse to one handle, so the pipelines built on them also dedup.
- **compiled shader** = source + entry point + stage + model + every compile option.

## Async: the build runs off the frame path

`acquire_compute_pipeline` and `shader_cache::compile` return an async handle, not a finished object,
because the work behind them (driver PSO lowering, DXC compilation) is multi-millisecond. These build on
[`cc::async`](../../../../base/clean-core/docs/async.md): the returned node is **scheduled onto the
installed default async pool** (`cc::install_default_async_pool`), and if no pool is installed it is driven
inline the first time you block on it. So the same handle works whether or not a thread pool exists.

The result types are the `sg::async_*` typedefs (`async_compiled_shader`, `async_compute_pipeline`).
`cc::async<T>` cannot hold a `const T` (its internal `cc::optional<T>` forbids it), so const arrives at the
**read** side: `try_value()` yields the const `*_handle`, and `cc::async_blocking_get` yields the handle by
value. A build failure surfaces as an **async error** on the node (`has_error()`), carrying the DXC / PSO
diagnostics — it is not thrown.

> **Threading caveat.** The async pipeline build calls a *backend* create from a pool worker. That is only
> safe where the backend permits concurrent pipeline creation (dx12 device creates are free-threaded). On a
> `single_threaded` [thread_model](threading.md), install no pool and let `cc::async_blocking_get` drive the
> build inline on the main thread.

Binding-layout acquisition stays **synchronous** — layout creation is cheap (a root signature), so paying
for an async node would be pure overhead.

## Tiers: in-memory now, disk/network later

The cache is a [`cc::key_value_cache<K, V>`](../../../../base/clean-core/src/clean-core/container/key_value_cache.hh):
a thread-safe stack of **provider tiers** consulted front (fastest) to back. The first tier to hit
backfills the faster tiers that missed; a full miss runs the factory and writes every tier. Only an
in-memory tier ships today (a `std::unordered_map` that clears wholesale past a capacity, driven by
`apply_bookkeeping`), but the `key_value_provider` interface is the seam for a disk-backed or networked
tier to drop in **without touching any call site** — the reason the tiered shape exists before there is a
second tier. Reach the cache via `ctx.cached.cache()` to install extra tiers or run bookkeeping.

## Flow

```
shader source ─▶ shader_cache.compile ─▶ async_compiled_shader ─┐
                                                                 ▼  (blocking_get / try_value)
bindings + static samplers ─▶ ctx.cached.acquire_binding_layout ─▶ binding_layout_handle ─┐
                                                                                           ▼
                              ctx.cached.acquire_compute_pipeline({shader, layout}) ─▶ async_compute_pipeline
                                                                                           │
                                                                          (blocking_get)   ▼
                                                                          compute_pipeline_handle ─▶ cmd.compute.bind_pipeline
```

Every arrow that names a cache is a get-or-create: identical inputs reuse the stored entry — and an
in-flight async is shared too, so a second `acquire` for a still-compiling pipeline hands back the same
node rather than starting a duplicate build.

## Deferred

Graphics and raytracing pipeline caching (sg has no such pipeline types yet), a content hash on
`compiled_shader` (so the pipeline key need not re-hash the bytecode), disk-backed provider tiers, and
richer eviction than clear-on-overflow.

## See also

- [pipeline_cache.hh](../../src/shaped-graphics/pipeline_cache.hh) — the layout + compute-pipeline cache.
- [context.cached.hh](../../src/shaped-graphics/context.cached.hh) / [context.uncached.hh](../../src/shaped-graphics/context.uncached.hh) — the `ctx.cached` / `ctx.uncached` scopes.
- [key_value_cache.hh](../../../../base/clean-core/src/clean-core/container/key_value_cache.hh) / [byte_stream_builder.hh](../../../../base/clean-core/src/clean-core/container/byte_stream_builder.hh) — the tiered cache + key serializer.
- [cc::async](../../../../base/clean-core/docs/async.md) — the async/dataflow system the async builds run on.
- [bindings](bindings.md) — the schemas being cached and where `binding_group` (not cached) lives.
