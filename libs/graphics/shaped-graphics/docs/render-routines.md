# Render routines

A **render routine** is a reusable, self-contained unit of GPU work — a post-process pass, a mipmap
generate, a LUT bake, a texture copy — that owns its own lazy, hot-reload-aware initialization. You
reach one by its C++ type and call it; it takes care of loading itself, rebuilding on a shader reload,
and living for exactly as long as the context it was built on.

The framework lives in **shaped-graphics** (`sg::render_routine`, `ctx.routines`, `sg::reload_generation`).
Concrete routines — the actual algorithms — live in **shaped-rendering** (`sr`), built on top of it.

## A routine: three-phase init

Derive from the CRTP base `sg::render_routine<Derived>` and override up to three phases, kept apart so
async pipeline compilation can start long before a command list exists:

| Phase | When | For |
|---|---|---|
| `init_once(ctx)` | first init only, **never** on reload | persistent work independent of shader content (e.g. a CPU-computed noise buffer uploaded once) |
| `init_declare(ctx)` | first init + after every reload | acquire shaders, acquire async pipelines (kicking off background compiles), start uploads. **No GPU work, no command recording.** |
| `init_materialize(cmd)` | first init + after every reload | record GPU init work (dispatches, clears, LUT bakes) |

Most routines need only `init_declare`; the other two default to no-ops. Re-init is driven by sg's
**process-global reload generation** (`sg::reload_generation()`): when content derived state is
invalidated it moves (the shader library calls `sg::signal_reload()` on hot reload), and the next
`acquire` re-runs declare + materialize while `init_once` state is preserved.

The customary call shape is a **static `execute()`** taking the command list. It opens with
`acquire(cmd)` — which finds (or lazily creates) this routine's per-context instance, initializes it,
and hands it back — and reads the routine's private members through that reference:

```cpp
class pattern_fill_routine : public sg::render_routine<pattern_fill_routine>
{
public:
    static void execute(sg::command_list& cmd, sg::buffer<sg::u32> const& out)
    {
        auto const& self = acquire(cmd);                 // lazily creates + initializes; returns the routine
        auto const pipeline = cc::async_blocking_get_singlethreaded(self._pipeline);
        auto const group = cmd.context().transient.create_binding_group(
            self._group_layout, {{.name = "gValues", .view = out.as_readwrite_buffer()}});
        cmd.compute.bind_pipeline(*pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(out.element_count());
    }

protected:
    void init_declare(sg::context& ctx) override
    {
        auto const shader = my::shaders::pattern_fill.compute.main->acquire(ctx);
        (void)cc::try_async_blocking_get_singlethreaded(shader);      // drive it (or install an async pool)
        auto const* const compiled = shader->try_value();
        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});
        _pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout});
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};

// call site — reach it by type, no handle, no registration:
pattern_fill_routine::execute(cmd, out);
```

`acquire(cmd)` reaches the context through `cmd.context()`, so it takes only the command list.
`prewarm(ctx)` is the variant for before a command list exists — it runs init_once + init_declare, so
async compiles start as early as possible; materialize then happens on the first `acquire(cmd)`.

## Per-context, reached by type: `ctx.routines`

A routine is a **per-context singleton**. Its instances live in the context's `routine_registry`,
reached as `ctx.routines`, a per-context sub-object like `ctx.cached`. The first `acquire` of a given
type creates and registers the instance there (lazy self-registration — no explicit registration call,
no by-name lookup); it lives until the context is shut down or you evict it.

Because instances live on the context, a routine's cached GPU state **dies with the context that built
it** — a routine can never hand stale, wrong-context handles to a second context. Switching between
contexts, or destroying and recreating one (as tests do), just works: the new context starts with an
empty registry and rebuilds from scratch.

The registry itself is not the API — everything type-keyed is reached through the routine's own
statics, and only `clear()` is public on `ctx.routines`:

```cpp
bloom_routine::prewarm(ctx);    // create + init_once/init_declare, before a command list exists
tonemap_routine::prewarm(ctx);
bloom_routine::evict(ctx);      // drop one routine's instance + its cached GPU state
ctx.routines.clear();           // drop all (VRAM pressure / context switch)
```

`prewarm` is the opt-in fan-out for startup: prewarming a set of routines kicks off all their async
pipeline compiles at once, so they build in parallel on the installed async pool
(`cc::install_default_async_pool`). There is no dependency graph — a routine that composes others just
calls them; lazy registration covers correctness, and you prewarm the leaves you care about.
`clear()` runs automatically on context shutdown.

`acquire` hits a per-thread, per-routine-type memo of the last instance handed out, so the steady
state is a pointer compare rather than a locked map lookup. The memo holds only a weak reference, so
it can never keep a routine alive past `evict` / `clear` / context shutdown — expiry is what
invalidates it.

Map access is guarded, so `acquire` is safe from parallel command-list recording. (Initializing a
*single* routine concurrently from two threads is not yet synchronized — a follow-up for when parallel
`init_declare` lands; today `prewarm` runs the declares sequentially and the async *compiles* are what
parallelize. Do not `clear()`/`evict()` a registry while another thread is still recording against the
same context.)

### Parametrized routines

The registry keys instances by type — one instance per routine type. A routine that legitimately needs
several instances with distinct resources (uncommon) will key on `{type, params}`; that overload is an
additive follow-up and is not built yet.

## Hot reload

Reload tracking is a single process-global counter in sg. The app sets up its `slib::shader_library`
and starts hot reload as usual; when a shader reloads, the library calls `sg::signal_reload()` and
`sg::reload_generation()` moves. Every routine reads that counter, so the affected routines re-run
`init_declare` + `init_materialize` on the next `acquire`. sg itself does not consume the counter — its
pipeline cache is content-keyed and rebuilds on its own — it only owns the counter as the lowest common
meeting point between the producer (the shader library) and the consumers (routines). There is only ever
one live shader library, which is why a single global counter suffices.

## See also

- [shaders.md](shaders.md) — the shader system a routine's `init_declare` pulls from.
- [concepts/caches.md](concepts/caches.md) — `ctx.cached.acquire_*`, the layout/pipeline caches a
  routine builds on.
- [shaped-rendering](../../shaped-rendering/readme.md) — where concrete routines live.
- [cheat-sheet.md](../cheat-sheet.md) — the sg public API at a glance.
