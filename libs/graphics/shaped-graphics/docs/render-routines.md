# Render routines

A **render routine** is a reusable, self-contained unit of GPU work — a post-process pass, a mipmap generate, a LUT bake, a texture copy — that owns its own lazy, hot-reload-aware initialization.
You reach one by its C++ type and call it; it takes care of loading itself, rebuilding on a shader reload, and living for exactly as long as the context it was built on.

The framework lives in **shaped-graphics** (`sg::render_routine`, `ctx.routines`, `sg::reload_generation`).
Concrete routines — the actual algorithms — live in **shaped-rendering** (`sr`), built on top of it.

## A routine: three-phase init

Derive from the CRTP base `sg::render_routine<Derived>` and override up to three phases, kept apart so async pipeline compilation can start long before a command list exists:

| Phase | When | For |
|---|---|---|
| `init_once(ctx)` | first init only, **never** on reload | persistent work independent of shader content (e.g. a CPU-computed noise buffer uploaded once) |
| `init_declare(ctx)` | first init + after every reload | acquire shaders, acquire async pipelines (kicking off background compiles), start uploads. **No GPU work, no command recording.** |
| `init_materialize(cmd)` | first init + after every reload | record GPU init work (dispatches, clears, LUT bakes) |

Most routines need only `init_declare`; the other two default to no-ops.
Re-init is driven by sg's **process-global reload generation** (`sg::reload_generation()`):
when content derived state is invalidated it moves (the shader library calls `sg::signal_reload()` on hot reload), and the next `acquire` re-runs declare + materialize while `init_once` state is preserved.

The customary call shape is a **static `execute()`** taking the command list.
It opens with `acquire(cmd)` — which finds (or lazily creates) this routine's per-context instance, initializes it, and hands it back —
and reads the routine's private members through that reference:

```cpp
class pattern_fill_routine : public sg::render_routine<pattern_fill_routine>
{
public:
    static void execute(sg::command_list& cmd, sg::buffer<sg::u32> const& out)
    {
        auto& self = acquire(cmd);                       // lazily creates + initializes; returns the routine
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
`prewarm(ctx)` is the variant for before a command list exists — it runs init_once + init_declare, so async compiles start as early as possible;
materialize then happens on the first `acquire(cmd)`.

The example above is deliberately the minimum, and it assumes one recording thread: its members are touched without a lock.
A routine recorded from several threads — or one holding anything that changes after init — needs the `cc::mutex<state>` shape from [Threading](#threading) below.

## Per-context, reached by type: `ctx.routines`

A routine is a **per-context singleton**.
Its instances live in the context's `routine_registry`, reached as `ctx.routines`, a per-context sub-object like `ctx.cached`.
The first `acquire` of a given type creates and registers the instance there (lazy self-registration — no explicit registration call, no by-name lookup);
it lives until the context is shut down or you evict it.

Because instances live on the context, a routine's cached GPU state **dies with the context that built it** — a routine can never hand stale, wrong-context handles to a second context.
Switching between contexts, or destroying and recreating one (as tests do), just works: the new context starts with an empty registry and rebuilds from scratch.

The registry itself is not the API — everything type-keyed is reached through the routine's own statics, and only `clear()` is public on `ctx.routines`:

```cpp
bloom_routine::prewarm(ctx);    // create + init_once/init_declare, before a command list exists
tonemap_routine::prewarm(ctx);
bloom_routine::evict(ctx);      // drop one routine's instance + its cached GPU state
ctx.routines.clear();           // drop all (VRAM pressure / context switch)
```

`prewarm` is the opt-in fan-out for startup:
prewarming a set of routines kicks off all their async pipeline compiles at once, so they build in parallel on the installed async pool (`cc::install_default_async_pool`).
There is no dependency graph — a routine that composes others just calls them; lazy registration covers correctness, and you prewarm the leaves you care about.
`clear()` runs automatically on context shutdown.

`acquire` hits a per-thread, per-routine-type memo of the last instance handed out, so the steady state is a pointer compare rather than a locked map lookup.
The memo holds only a weak reference, so it can never keep a routine alive past `evict` / `clear` / context shutdown — expiry is what invalidates it.

## Threading

A routine is a per-context singleton handed to every caller on that context, so the threading model has to be explicit.
It is three separate guarantees, and only the first two are the framework's:

1. **The registry is guarded.** `acquire` is safe from parallel command-list recording.
2. **The phase engine is guarded.** Racing acquires of one routine run each phase exactly once — the losers block until the winner is done, then observe it initialized.
   The phase callbacks therefore run under that lock, and must not call back into `acquire` / `prewarm` for the same routine.
3. **A routine's own mutable state is the routine's job.** The framework cannot know what a routine keeps or how it wants it synchronized.

That third point is the one that bites.
`acquire` returns a **non-const** reference precisely because routines are expected to hold state — a pipeline cache keyed by target format, a resource registry, a scratch buffer that grows.
Anything `execute` writes is written through a reference two threads may hold at once.

The shape to reach for first is a single `cc::mutex<state>` holding everything the routine owns, locked once per entry point:

```cpp
class my_routine : public sg::render_routine<my_routine>
{
public:
    static void execute(sg::command_list& cmd, /* args */)
    {
        auto& self = acquire(cmd);
        self._state.lock([&](state& s) { /* read and write s freely */ });
    }

protected:
    void init_declare(sg::context& ctx) override
    {
        _state.lock([&](state& s) { /* rebuild the shader-derived half of s */ });
    }

private:
    struct state { /* pipelines, layouts, caches, per-frame scratch */ };
    cc::mutex<state> _state;
};
```

One mutex over everything keeps the rule checkable by inspection, and it costs nothing real: two threads recording the same routine serialize, which is what they would have to do anyway.

**State written in `init_declare` and only read afterwards is not exempt.**
A reload on another thread re-runs `init_declare` while this thread is recording, so those members belong behind the same mutex as the rest.
Note the lock order this implies — the phase lock is taken first, then the routine's own — so a routine must never take its own lock and then call `acquire`.

Do not `clear()` / `evict()` a registry while another thread is still recording against the same context.

### Known gap: submission order is not modeled

The three guarantees cover *recording*, not *submission*, and that is not yet sound.
A routine can be recorded into command list A and then, in sequence, into command list B —
each recording correct in isolation —
yet leave an implicit GPU-ordering dependency between the two: something the routine owns that B's work assumes A's has already run (an upload it recorded, a resource transition, a buffer it grew).
The framework enforces nothing about the order those lists are *submitted*.
Submit B before A and the dependency inverts — the result is wrong even though every lock was held correctly.

So the model does not yet cover a routine recorded across several command lists whose submission order differs from their recording order.
Until it does, keep such a routine's dependent work within one list, or submit the lists in the order they were recorded.

### Parametrized routines

The registry keys instances by type — one instance per routine type.
A routine that legitimately needs several instances with distinct resources (uncommon) will key on `{type, params}`; that overload is an additive follow-up and is not built yet.

## Hot reload

Reload tracking is a single process-global counter in sg.
The app sets up its `slib::shader_library` and starts hot reload as usual; when a shader reloads, the library calls `sg::signal_reload()` and `sg::reload_generation()` moves.
Every routine reads that counter, so the affected routines re-run `init_declare` + `init_materialize` on the next `acquire`.
sg itself does not consume the counter — its pipeline cache is content-keyed and rebuilds on its own —
it only owns the counter as the lowest common meeting point between the producer (the shader library) and the consumers (routines).
There is only ever one live shader library, which is why a single global counter suffices.

## See also

- [shaders.md](shaders.md) — the shader system a routine's `init_declare` pulls from.
- [concepts/caches.md](concepts/caches.md) — `ctx.cached.acquire_*`, the layout/pipeline caches a routine builds on.
- [shaped-rendering](../../shaped-rendering/readme.md) — where concrete routines live.
- [cheat-sheet.md](../cheat-sheet.md) — the sg public API at a glance.
