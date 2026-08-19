# Concept: caches (layouts, pipelines, shaders)

Building a `binding_group_layout`, a `pipeline_layout`, or a `compute_pipeline` is expensive and repetitive.
The same shader is compiled again and again, structurally identical layouts and PSOs are rebuilt, and a driver-level PSO build takes multiple milliseconds.
sg answers this with a **content-addressed, get-or-create cache** for the pipeline schemas, and the shader compiler with a matching cache for compiled shaders.
The verb is **`acquire`** — "get it if it exists, otherwise create and store it".

## Three layers: raw, cached, and the async result

```
ctx.uncached.create_*   ── raw, always builds a fresh backend object (the escape hatch)
ctx.cached.acquire_*    ── get-or-create over the built-in pipeline_cache  ◀── prefer this
ssc::dxc::shader_cache  ── get-or-create over the DXC compiler (a separate lib)
```

- [`ctx.uncached`](../../src/shaped-graphics/context/uncached.hh) is the **raw layer**: it builds a fresh layout or pipeline on every call.
  It sits apart from `ctx.persistent` / `ctx.transient` because a layout or pipeline is a schema, not a lifetime-scoped GPU resource — see [context](context.md).
  Uncached is a **deliberately poor default**: most code should not rebuild these per frame.
- [`ctx.cached`](../../src/shaped-graphics/context/cached.hh) is the **front door**.
  Every context owns a [`pipeline_cache`](../../src/shaped-graphics/context/pipeline_cache.hh), with default in-memory tiers installed, reached here.
  `acquire_binding_group_layout` and `acquire_pipeline_layout` return shared handles.
  `acquire_compute_pipeline` and `acquire_raytracing_pipeline` return an **async** handle whose build runs off-thread.
- The DXC [`shader_cache`](../../../shaped-shader-compiler-dxc/src/shaped-shader-compiler-dxc/shader_cache.hh) lives in the compiler library, not in sg — sg has no compiler.
  It caches `compile()` the same way: the same request yields the same async compiled shader, never recompiled.

`binding_group` is **not** here — it is a real per-instance descriptor allocation and stays on `ctx.persistent` / `ctx.transient` (see [bindings](bindings.md)).

## The key is the content, not the handle

A cache entry is keyed by a [`cc::hash128`](../../../../base/clean-core/src/clean-core/common/hash128.hh) computed from the **logical creation arguments**.
So the key is independent of any backend handle identity, and stable across runs.
The arguments are serialized into a [`cc::byte_stream_builder`](../../../../base/clean-core/src/clean-core/container/byte_stream_builder.hh), then hashed (XXH3-128).
That builder length-prefixes each piece, which is what keeps different splits of the same data distinct.
Sub-structs are hashed field by field, never as a raw `memcpy` of a struct whose padding bytes would be nondeterministic.

- **binding group layout** = the reflected `binding`s **plus the static samplers**.
  Static samplers are baked into the root signature, so a different static sampler is a different group layout and must be part of the key.
- **pipeline layout** = its groups' own **structural hashes**, plus its register-bound static samplers and its inline constants.
  All three change the root signature.
  A layout carries that hash from creation — `layout->structural_hash()` — computed once by the backend through the same `sg::impl` functions this cache keys with, so the two can never disagree.
- **compute pipeline** = the shader's content (bytecode + entry point + compiler signature) combined with the **pipeline layout's structural hash**, which transitively covers its group layouts.

- **compiled shader** = source + entry point + stage + model + every compile option + **the DXC version**.
  The version matters only to a key that outlives the process, and there it is load-bearing: without it a DXC upgrade keeps serving the previous compiler's DXIL forever.

Structural rather than pointer identity is what makes these keys mean anything outside the process that made them.
Two independently created but identical group layouts collapse to one key, so acquiring through the cache is a convenience rather than a precondition for dedup.
A key computed in one run also still names the same thing in the next — the property a persistent tier needs, and one an address could never have given it.

## The persistent tier

A pipeline that misses in memory consults [blob-cache](../../../../data/blob-cache/docs/design.md) for a serialized PSO blob before building, and stores the one it produced on the way out.
The DXC shader cache does the same with encoded `compiled_shader`s.
So the in-memory tier holds live handles for this run, and bcache holds bytes for every run after it.

The two differ in one way that shapes the code.
For a shader the cacheable product and the expensive product are the same bytes, so it is a plain `acquire`: decode what comes back, compile and encode on a miss.
For a pipeline they are not: the store holds a blob while the caller wants a live handle.
So the singleflight's winner stashes what it built in a slot, which then doubles as the hit/miss signal `acquire` does not otherwise expose.
Shaders go through [`sg::impl::encode_compiled_shader`](../../src/shaped-graphics/binding/impl/shader_codec.hh), a codec with its own version prefix.
It refuses anything doubtful, because a cache may miss but must never lie.

It is deliberately **not** a `cc::key_value_provider` tier, and it could not be one: a provider's `try_get` is synchronous and runs under the cache's lock, while bcache is async.
Blocking there would stall every concurrent acquire.
The store is consulted inside the miss build instead, through `bcache::acquire`, so the whole lookup-build-store pipeline singleflights and identical blobs deduplicate by content.

The key is the in-memory key plus the **adapter and driver**, because a blob is only valid for the pair that wrote it.
Under-keying is cheap by construction: a blob the driver refuses costs one failed create and nothing else, since the backend retries without it.

**Staleness is observed, never predicted.**
`used_cached_pipeline()` reports whether creation actually consumed the blob, and a refusal is what triggers replacing the entry.
The tempting alternative is comparing the blob the pipeline hands back against the one that was stored, and it is wrong.
A real driver re-serializes an accepted blob to different bytes, so that test would rewrite every entry on every run.
It would also look perfectly healthy on WARP, which reproduces the bytes exactly.

**It needs somewhere to schedule.**
A build parks on the store, so it has to be able to resume somewhere.
With no ambient scheduler installed and no worker scope active there is nowhere, and the tier is skipped rather than parked — the pipeline is built the plain way.

**Without threads, blocking still works.**
A store with no thread of its own registers a pump with clean-core, and `cc::async_blocking_get` sweeps that registry instead of sleeping.
So the same blocking call drives the store and then resumes the build, and there is no threadless code path to write: one `cc::async_blocking_get` is correct in both builds.

`ctx.cached.cache().set_blob_cache(...)` overrides the store per context; it defaults to `bcache::default_cache()`, and `nullptr` turns persistence off.
Tests share the developer's real cache like anything else, and are faster for it — most of them only want a pipeline, not a cold build of one.
A test that is *about* caching opens its own store and passes it here.
`SC_BLOB_CACHE` turns the default off or redirects it to temp for a whole run, which is the lever for asking whether a stale entry is behind a result.

## Async: the build runs off the frame path

`acquire_compute_pipeline` and `shader_cache::compile` return an async handle rather than a finished object, because driver PSO lowering and DXC compilation are multi-millisecond work.
Both build on [`cc::async`](../../../../base/clean-core/docs/systems/async.md): the returned node is **scheduled onto the ambient scheduler** (`cc::install_default_async_scheduler`).
Blocking on it drives it there, so the same handle works whether that scheduler is a pool or a single-threaded one.

The result types are the `sg::async_*` typedefs (`async_compiled_shader`, `async_compute_pipeline`, `async_raytracing_pipeline`).
`cc::async<T>` cannot hold a `const T` — its internal `cc::optional<T>` forbids it — so const arrives at the **read** side.
`try_value()` yields the const `*_handle`, and `cc::async_blocking_get` yields the handle by value.
A build failure surfaces as an **async error** on the node (`has_error()`) carrying the DXC / PSO diagnostics; it is not thrown.

> **Threading caveat.** The async pipeline build calls a *backend* create from a pool worker.
> That is only safe where the backend permits concurrent pipeline creation — dx12 device creates are free-threaded.
> On a `single_threaded` [thread_model](threading.md), make the ambient scheduler a `cc::singlethreaded_scheduler`, and the build runs inline on whichever thread blocks on it.

Binding-layout acquisition stays **synchronous**: layout creation is cheap (a root signature), so paying for an async node would be pure overhead.

## Tiers: in-memory now, disk/network later

The cache is a [`cc::key_value_cache<K, V>`](../../../../base/clean-core/src/clean-core/container/key_value_cache.hh), a thread-safe stack of **provider tiers** consulted front (fastest) to back.
The first tier to hit backfills the faster tiers that missed; a full miss runs the factory and writes every tier.
Only an in-memory tier ships today — a `std::unordered_map` that clears wholesale past a capacity, driven by `apply_bookkeeping`.
The `key_value_provider` interface is the seam for a disk-backed or networked tier to drop in **without touching any call site**, which is why the tiered shape exists before there is a second tier.
Reach the cache via `ctx.cached.cache()` to install extra tiers or run bookkeeping.

## Flow

```
shader source ─▶ shader_cache.compile ─▶ async_compiled_shader ─────────────────────────────────┐
                                                                                                  ▼  (blocking_get / try_value)
bindings + static samplers ─▶ ctx.cached.acquire_binding_group_layout ─▶ binding_group_layout_handle ─┐
                                                                                                  ▼   │
                              ctx.cached.acquire_pipeline_layout({groups}) ─▶ pipeline_layout_handle ─┤
                                                                                                  ▼
                              ctx.cached.acquire_compute_pipeline({shader, layout}) ─▶ async_compute_pipeline
                                                                                                  │
                                                                                 (blocking_get)   ▼
                                                                                 compute_pipeline_handle ─▶ cmd.compute.bind_pipeline
```

Every arrow that names a cache is a get-or-create: identical inputs reuse the stored entry.
An in-flight async is shared too, so a second `acquire` for a still-compiling pipeline hands back the same node rather than starting a duplicate build.

## Deferred

**Raster-pipeline caching.** The type exists (see [raster pipeline](raster-pipeline.md)) but `ctx.cached` has no `acquire_raster_pipeline` yet, unlike compute and raytracing.
**A content hash on `compiled_shader`**, so a pipeline key need not re-hash the bytecode.
**Disk-backed provider tiers**, and richer eviction than clear-on-overflow.

## See also

- [context](context.md) — why layouts and pipelines sit on their own scopes rather than on `ctx.persistent` / `ctx.transient`.
- [pipeline_cache.hh](../../src/shaped-graphics/context/pipeline_cache.hh) — the layout + pipeline cache itself.
- [cached.hh](../../src/shaped-graphics/context/cached.hh) / [uncached.hh](../../src/shaped-graphics/context/uncached.hh) — the `ctx.cached` / `ctx.uncached` scopes.
- [key_value_cache.hh](../../../../base/clean-core/src/clean-core/container/key_value_cache.hh) — the tiered cache behind it.
- [cc::async](../../../../base/clean-core/docs/systems/async.md) — the async/dataflow system the async builds run on.
- [bindings](bindings.md) — the schemas being cached, and where `binding_group` (not cached) lives.
