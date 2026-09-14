# Render routines

A **render routine** is a reusable, self-contained unit of GPU work — a post-process pass, a mipmap generate, a LUT bake, a texture copy — that owns its own lazy, hot-reload-aware initialization.
You reach one by its C++ type and call it; it takes care of loading itself, rebuilding on a shader reload, and living for exactly as long as the context it was built on.

The framework lives in **shaped-graphics** (`sg::render_routine`, `ctx.routines`, `sg::reload_generation`).
Concrete routines — the actual algorithms — live in **shaped-rendering** (`sr`), built on top of it.

## A routine: two-phase init

Derive from the CRTP base `sg::render_routine<Derived>` and override the phases you need.
Both are **coroutines** returning `cc::shared_async<cc::unit>`, so a phase awaits the shader compiles and pipeline builds it needs instead of holding a thread against them:

| Phase | When | For |
|---|---|---|
| `init_once(scope)` | first init only, **never** on reload | persistent work independent of shader content (e.g. a CPU-computed noise buffer uploaded once) |
| `init(scope)` | first init + at every reload generation | acquire shaders, build pipelines, declare dependencies, record GPU init work |

Both default to a no-op, so a routine names only what it needs.
Re-init is driven by sg's **process-global reload generation** (`sg::reload_generation()`):
the shader library calls `sg::signal_reload()` on hot reload, and the next tick re-runs `init` while `init_once` state is preserved.
An in-flight `init` is cancelled when the generation moves; `init_once` never is, because restarting it would either lose work or make the name a lie.

The scope is a **value type, taken by value**, and that is not a style choice — a coroutine's reference parameter dangles across its first suspend.
It carries the context, the generation this run belongs to, and the two things a phase awaits:

- **`co_await scope.with_cmd(f)`** records GPU init work into the tick's shared command list.
  It is *awaited*, not merely called.
  A phase may resume on any worker long after the tick that started it returned, so this parks until a window is open rather than reaching for a list that is not there.
  `f` takes a `sg::command_list&` and is not itself a coroutine, which is what keeps a list from being held across an await — there is nowhere inside it to await.
- **`co_await scope.yield()`** is a budget boundary inside one phase, and a cancellation check.
  The tick's budget is checked between routines, so a phase that knows its work is chunky awaits this between chunks and becomes pacable.

A failed shader is a **verdict this routine reports**, not an error to propagate.
So a phase awaits it with `cc::async_settled` and calls `fail_init()` — see [Readiness](#readiness-and-what-a-routine-reports) below.
`co_await` on its own short-circuits, which is right for work whose failure really is this phase's failure.

The customary call shape is a **static `execute()`** taking the command list, opening with `try_acquire` / `try_acquire_exclusive` and branching once:

```cpp
class pattern_fill_routine : public sg::render_routine<pattern_fill_routine>
{
public:
    static void execute(sg::command_list& cmd, sg::buffer<sg::u32> const& out)
    {
        auto const self = try_acquire(cmd);
        if (!self.is_ready())
            return;                                     // still building, or it failed — see routine_readiness

        // Polled, never waited on: init awaited the build, so a ready routine has a built pipeline.
        auto const pipeline = *self->_pipeline->try_value();
        auto const group = cmd.context().transient.create_binding_group(
            self->_group_layout, {{.name = "gValues", .view = out.as_readwrite_buffer()}});
        cmd.compute.bind_pipeline(*pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(out.element_count());
    }

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override
    {
        auto& ctx = scope.context();
        auto const shader = my::shaders::pattern_fill.compute.main->acquire(ctx);
        co_await cc::async_settled(shader);             // settled: a shader that will not build is ours to report
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
        {
            fail_init();                                // failed, not pending — a caller can tell the two apart
            co_return;
        }

        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});
        _pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout});
        co_await cc::async_settled(_pipeline);          // awaited HERE, so `ready` means ready
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};

// call site — reach it by type, no handle, no registration:
pattern_fill_routine::execute(cmd, out);
```

Both entry points reach the context through `cmd.context()`, so they take only the command list; an overload taking `sg::context&` covers the sites where no list exists yet.
`prewarm(ctx)` only *registers* a routine so the next tick brings it up — it builds nothing itself.

## Per-context, reached by type: `ctx.routines`

A routine is a **per-context singleton**.
Its instances live in the context's `routine_registry`, reached as `ctx.routines`, a per-context sub-object like `ctx.cached`.
The first `try_acquire` or `prewarm` of a given type registers the instance there (lazy self-registration — no explicit registration call, no by-name lookup);
it lives until the context is shut down or you evict it.
A **parametrized** routine is keyed on `(type, hash(params))` instead, so one instance exists per distinct parameter value — see [Parametrized routines](#parametrized-routines).

Because instances live on the context, a routine's cached GPU state **dies with the context that built it** — a routine can never hand stale, wrong-context handles to a second context.
Switching between contexts, or destroying and recreating one (as tests do), just works: the new context starts with an empty registry and rebuilds from scratch.

The registry itself is not the API — everything type-keyed is reached through the routine's own statics, and only `clear()` is public on `ctx.routines`:

```cpp
bloom_routine::prewarm(ctx);       // register it, so the NEXT tick brings it up
tonemap_routine::prewarm(ctx);
blit_routine::prewarm(ctx, fmt);   // one parametrization of a parametrized routine
bloom_routine::evict(ctx);         // drop one instance + its cached GPU state
blit_routine::evict_all(ctx);      // drop every parametrization of one routine
ctx.routines.clear();              // drop all (VRAM pressure / context switch)
```

`prewarm` is the opt-in fan-out for startup, and it **builds nothing itself**.
Prewarming a set of routines is how a whole frame's worth of compiles start in one tick, instead of being discovered one acquire at a time over the following frames.
A routine that composes others declares those edges in its own `init` (see [Dependency tokens](#dependency-tokens)), so prewarming the top of a renderer covers everything under it.
`clear()` runs automatically on context shutdown.

Acquiring hits a per-thread memo of the last instance handed out, keyed on `(context, params hash)`, so the steady state is a pointer compare rather than a locked map lookup.
The memo holds only a weak reference, so it can never keep a routine alive past `evict` / `clear` / context shutdown — expiry is what invalidates it.

## Threading

A routine is a per-context singleton handed to every caller on that context, so the threading model has to be explicit.
Three guarantees, all of them the framework's:

1. **The registry is guarded.** Acquiring is safe from parallel command-list recording.
2. **Initialization runs only inside a tick**, which is a frame-boundary call — so the phases are never concurrent with each other, and a reload cannot land in the middle of a frame.
3. **`try_acquire_exclusive` hands the routine's lock to the caller**, so a routine carries no mutex of its own and its executes serialize against each other.

What the lock does **not** cover any more is initialization.
A coroutine cannot hold a `cc::mutex` across a `co_await`, so a routine's own members are written by `init` without it, and **readiness is the publication barrier instead**:
the `pending -> ready` transition the tick publishes is what a reader's acquire synchronizes with, so a routine handed out as ready has everything its phases wrote visible.

The consequence worth knowing is a behavioural one: a hot reload now shows as a few pending frames rather than a multi-millisecond hitch.
Nothing blocks on a recompile — the frames in between simply skip the routine.

Which entry point you use is how a routine declares whether it mutates:

```cpp
class my_routine : public sg::render_routine<my_routine>
{
public:
    static void execute(sg::command_list& cmd, /* args */)
    {
        auto self = try_acquire_exclusive(cmd);   // holds the routine's lock for the guard's lifetime
        if (!self.is_ready())
            return;
        self->_scratch.grow(...);                 // read and write freely through ->
    }

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override
    {
        _group_layout = ...;                      // NOT under the lock; readiness publishes it
        co_return;
    }

private:
    sg::binding_group_layout_handle _group_layout;   // plain members — no `struct state`, no cc::mutex
};
```

`try_acquire_exclusive` is the one to reach for.
A routine is expected to hold state — its pipeline, a resource registry, a scratch buffer that grows — and the guard is what makes writing it safe.
Keep the guard to the scope that actually mutates; it serializes every other thread recording that routine for as long as it lives.

`try_acquire` is the other half, and it takes **no lock at all**: it hands back a read-only scope, so only non-mutating members are reachable.
That makes it a contract rather than a guarantee — **whatever it can reach must be immutable after init, or self-guarded on its own**.
So **a routine whose `execute` touches anything `init` writes belongs on `try_acquire_exclusive`**, which in practice is nearly all of them.

**Neither one initializes.**
Asking registers the routine; [the tick](#the-tick-is-what-initializes) is what brings it up, which is why both report three states rather than handing back something usable unconditionally.

Taking a *different* routine's guard while holding your own is fine, and a dependency token is how it is spelled.
`self.acquire_exclusive(token)` hands back a guard on the dependency, taken after the holder's.
The order is holder then dependency, which the acyclic graph makes consistent — reaching the two the other way round would need a cycle, and those are refused where the edge is declared.
The lock is not recursive, so the rule that remains is: never re-acquire the *same* routine under its own guard.

The exclusive guard is an approximation of the model this actually wants, and the gap is a missing clean-core type:
**executes that only read should run in parallel with each other**, while the read-only scope should hold a shared lock rather than none.
A read-only routine like `sr::blit_routine` has no reason to serialize against another thread's `execute`, and `try_acquire`'s no-lock path is not the same thing as a shared one.
Closing it needs a `cc::shared_mutex<T>` beside `cc::mutex<T>`; it is tracked in the [sg TODO](TODO.md).

Do not `clear()` / `evict()` a registry while another thread is still recording against the same context.

## The tick is what initializes

Nothing a caller does brings a routine up.
`ctx.routines.tick()` does, and a routine nothing has ticked reads as pending.
That is the point: initialization is where shaders are compiled and pipelines built, and none of that may happen on the frame path.

It is a **frame-boundary call**.
It opens and submits a command list of its own, so it must not run inside one, and it belongs after `advance_epoch` and before the frame's first acquire.
Every routine it brings up in one tick records into that one list, so their GPU init work batches into a single submit.

**Initialization runs on the ambient async scheduler**, and installing one is the application's job (`cc::install_compute_async_scheduler`).
The tick asserts where there is none rather than standing up a private one.
A phase nothing can drive would leave every routine pending forever, which is a configuration error and not a state to report.
While it is driving, the tick participates in that scheduler, so a single-threaded one works too — the phases then run inline on the thread that ticked.

Its budget is **advisory pacing rather than a deadline**, and it bounds how long the *tick* spends driving, not how long an initialization takes.
A phase runs on a worker, so a budget below what the phases cost returns with them still in flight, and a later tick collects them.
The check sits between passes, so a tick still overruns by however long one uninterrupted stretch of a phase takes — `co_await scope.yield()` is how a phase makes itself pacable.
A caller that treats the budget as a frame deadline will eventually miss one and blame the wrong thing.

A reload is observed here too, and only here, so it cannot land in the middle of a frame.
An `init` in flight when the generation moves is cancelled and restarted at the new one; `init_once` is left alone.

**An application that never ticks gets a renderer where nothing is ever ready**, silently — every `try_acquire` pending, every draw skipped, an empty screen and no explanation.
That is a new way to hold the library wrong, so the registry says so: routines asked for a thousand times with no tick ever run is a warning, once, on sg's recording domain.

### Known gap: submission order is not modeled

The three guarantees cover *recording*, not *submission*, and that is not yet sound.
A routine can be recorded into command list A and then, in sequence, into command list B —
each recording correct in isolation —
yet leave an implicit GPU-ordering dependency between the two: something the routine owns that B's work assumes A's has already run (an upload it recorded, a resource transition, a buffer it grew).
The framework enforces nothing about the order those lists are *submitted*.
Submit B before A and the dependency inverts — the result is wrong even though every lock was held correctly.

So the model does not yet cover a routine recorded across several command lists whose submission order differs from their recording order.
Until it does, keep such a routine's dependent work within one list, or submit the lists in the order they were recorded.

### Readiness, and what a routine reports

`try_acquire` and `try_acquire_exclusive` hand back a scope or a guard carrying one of three states, spelled `is_ready()` / `is_pending()` / `is_failed()`.
There is deliberately **no `operator bool`**, so a call site says which one it is testing.

- **pending** — still building; come back after another tick.
- **ready** — every phase ran at the current reload generation, and so did every routine in its token subtree.
- **failed** — initialization will not succeed until something changes (a reload, an eviction).
  A phase says so with `fail_init()`, or by failing its node.

"Still compiling" and "will never compile" produce the same answer to a caller that only draws, and collapsing them is how a broken shader becomes a black rectangle that reports nothing.
The common branch is the same either way, so the third state costs nothing at the sites that ignore it, and it is the difference between a failure a test can assert on and one a person has to notice.

A **failed shader reload does not reach `fail_init`**.
slib promotes a recompile only when it produced a value, so a bad edit leaves the last good shader in place and does not even bump the generation.
What reaches it is a shader that was never good — a first compile failing, a missing package, or a context accepting no format any registered compiler produces.

The other half is what a routine reports back out of `execute`.
A routine whose dependency set is entirely static tokens is only handed out once its whole subtree is ready, so its `execute` returns `void`.
A routine that acquires something *dynamically* during execution can still decline, and it has to **say so**: it returns `[[nodiscard]] sg::routine_outcome`, `executed` or `declined`.
A caller that cannot tell "declined" from "nothing to draw" is how a headless capture writes a blank image and reports success.

### Dependency tokens

A routine that needs another one declares the edge during `init` and gets a token back:

```cpp
cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override
{
    _atlas = depend_on<atlas_routine>(scope.context());          // records the edge; returns the token
    _mipmap = depend_on<mipmap_routine>(scope.context(), fmt);   // a parametrized dependency
    co_return;
}
```

The framework then **refuses to hand out the holder until its whole token subtree is ready**, which is what makes redeeming one during execution infallible:

```cpp
auto const& atlas = self.acquire(_atlas);                // cannot fail
auto mip = self.acquire_exclusive(_mipmap);              // the same, for one this routine mutates
```

That is the point of the whole mechanism: **the number of readiness checks a renderer grows is one per entry into the routine system, not one per routine.**
Without it every composing routine would have to know the pending state of everything under it and do the right thing in each case, which is boilerplate and gets it wrong somewhere.

A token is redeemed **through the scope or the guard**, never off the routine, so "valid only while the holder is acquired" is structural rather than an assert that release compiles out.
It holds a **strong reference**, so a depended-on routine cannot be evicted while a dependent exists and redemption needs no liveness check.

A **cycle is refused where the edge is declared**, with an assert naming what closed it.
It would be two failures at once: readiness could never settle, and initialization would take the two routines' locks in opposite orders.
With assertions off the cycle is not caught — the readiness walk carries a visited set, so it costs a refcount leak rather than a hang.

### Parametrized routines

Some routines cannot exist as a single thing.
A raster pipeline bakes its color-target format in, so "blit" is one unit of GPU work *per format*; a mipmap compute shader is one *per texture shape*.
Such a routine is a **schema**, and the unit a caller wants is the schema applied to a concrete argument:

```cpp
class blit_routine : public sg::render_routine<blit_routine, sg::pixel_format> { ... params() ... };

blit_routine::try_acquire(cmd, scope.color_formats()[0]);   // the value picks the instance
```

The template names the parameter's **type**; the value is runtime.
A format is read off a texture or a swapchain, so a compile-time parameter would force every caller to switch over an enumeration to pick the instantiation — the thing routines exist to avoid.
The registry keys instances on `(type, hash(params))`, so there is one instance per distinct value, each owning the one pipeline it needs.
A parameter must be copyable and `==`-comparable; an enum hashes through its underlying value, anything else supplies an ADL `hash(p)`.

**A parameter must be drawn from a small, enumerable set.**
One instance per distinct value is held until the routine is evicted or the context dies.
So a routine parametrized on a texture *size*, or on anything a scene supplies, is an unbounded cache that looks like a design.
That is documented rather than enforced: a cap that fires is a routine that silently stops working, which is worse than a leak that shows up in a memory graph.

Nothing unused is built, which is the laziness a per-key pipeline cache used to provide.
A caller that only mips 2D textures never reaches the 1D, 3D or array shaders, because it never acquires those instances.

## Hot reload

Reload tracking is a single process-global counter in sg.
The app sets up its `slib::shader_library` and starts hot reload as usual; when a shader reloads, the library calls `sg::signal_reload()` and `sg::reload_generation()` moves.
Every routine reads that counter, so the affected routines re-run `init` at the next tick, while `init_once` state is preserved.
sg itself does not consume the counter — its pipeline cache is content-keyed and rebuilds on its own —
it only owns the counter as the lowest common meeting point between the producer (the shader library) and the consumers (routines).
There is only ever one live shader library, which is why a single global counter suffices.

## See also

- [shaders.md](shaders.md) — the shader system a routine's `init` pulls from.
- [concepts/caches.md](concepts/caches.md) — `ctx.cached.acquire_*`, the layout/pipeline caches a routine builds on.
- [shaped-rendering](../../shaped-rendering/readme.md) — where concrete routines live.
- [cheat-sheet.md](../cheat-sheet.md) — the sg public API at a glance.
