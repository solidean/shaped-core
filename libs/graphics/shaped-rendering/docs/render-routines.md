# Render routines

The composable building block sr is built from: a self-contained unit of GPU work — a post-process
pass, a mipmap generate, a LUT bake, a texture copy — that owns its own lazy, hot-reload-aware
initialization. Assemble a library of graphics algorithms out of them.

## The three types

```text
render_routine          one unit of GPU work; 3-phase init, re-inits itself on shader reload
render_routine_package  a hand-written group of routines you reach by name; can depend on other packages
render_routine_library  the one object you keep around: references the packages and drives fan-out init
```

This mirrors the shader system's [library/package split](../../shaped-graphics/docs/shaders.md): the
*library* is the runtime aggregator you build once, and *packages* are what you actually declare and
reach through. The difference is that a shader package is a generated static description, while a
**routine package is a live C++ object** — you write the class, and its routines are plain members.

## A routine: three-phase init

A routine subclasses `render_routine` and overrides up to three phases, kept apart so async pipeline
compilation starts long before a command list exists:

| Phase | When | For |
|---|---|---|
| `init_once(ctx)` | first init only, **never** on reload | persistent work independent of shader content (e.g. a CPU-computed noise buffer uploaded once) |
| `init_declare(ctx)` | first init + after every reload | acquire shaders, acquire async pipelines (kicking off background compiles), start uploads. **No GPU work, no command recording.** |
| `init_materialize(cmd)` | first init + after every reload | record GPU init work (dispatches, clears, LUT bakes) |

Most routines need only `init_declare`; the other two default to no-ops. Re-init is driven by slib's
**process-global shader-reload generation** (`slib::current_reload_generation()`): when a shader
reloads it moves, and the next `ensure_*` re-runs declare + materialize while `init_once` state is
preserved. A routine reads that global directly — it holds no library reference.

The customary call shape is a **static `execute()`** on the subclass, taking a `routine_handle`. The
method order then reads in run order, and `execute()` reaches the routine's private members through the
initialized reference the handle hands back:

```cpp
class pattern_fill_routine : public sr::render_routine
{
public:
    static void execute(sr::routine_handle<pattern_fill_routine> const& handle,
                        sg::command_list& cmd, sg::buffer<sg::u32> const& out)
    {
        auto const& self = handle.acquire(cmd);          // initializes if needed, then returns the routine
        auto const group = cmd.context().transient.create_binding_group(
            self._group_layout, {{"gValues", out.as_readwrite_buffer()}});
        cmd.compute.bind_pipeline(*self._pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(int(out.element_count()));
    }

protected:
    void init_declare(sg::context& ctx) override
    {
        auto const shader = my::shaders::pattern_fill.compute.main->acquire(ctx);
        (void)cc::try_async_blocking_get_singlethreaded(shader);      // drive it (or install an async pool)
        auto const* const compiled = shader->try_value();
        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});
        _pipeline = cc::async_blocking_get_singlethreaded(
            ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout}));
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::compute_pipeline_handle _pipeline;
};
```

`ensure_initialized(cmd)` and `acquire(cmd)` reach the context through `cmd.context()`, so they take
only the command list. (`ensure_initialized_no_materialize(ctx)` still takes the context — it is the
prewarm phase, run before any command list exists.)

## A package: routines as members, self-owned

A package groups related routines. Subclass `render_routine_package`, declare `routine_handle` members,
and populate them in `setup()` with `register_routine`. Depend on another package with `depend(...)`,
passing that package's shared handle — a package **does not know any library**; it just records its
dependencies so a library can walk them. It is customary (not required) to make a package a
process-wide singleton via a static `acquire()` built with `sr::make_package`:

```cpp
class postprocess_package : public sr::render_routine_package
{
public:
    static std::shared_ptr<postprocess_package> acquire()
    {
        static auto instance = sr::make_package<postprocess_package>(); // constructs + runs setup() once
        return instance;
    }
    sr::routine_handle<vignette_routine> vignette;

protected:
    void setup() override
    {
        _texops = texture_ops_package::acquire();
        depend(_texops);                                 // record the dependency for the closure walk
        vignette = register_routine<vignette_routine>();
    }
private:
    std::shared_ptr<texture_ops_package> _texops;        // keep the typed handle to use its routines
};
```

`sr::make_package<P>()` is the single construct-and-`setup()` path, with a **dependency-cycle guard**
(a cycle resolved through it asserts rather than recursing forever). The singleton `acquire()` wraps it
in a `static`; a test that wants a fresh, isolated instance calls `make_package` directly.

Because a package is a singleton, acquiring it anywhere returns the same instance — so a package (and
its routines, which often hold non-trivial GPU resources) is **deduplicated across every library and
every dependent** for free.

## The library: keep one around

```cpp
sr::render_routine_library lib;
lib.add_package(postprocess_package::acquire());   // + its transitive dependency closure, deduplicated

// prewarm before opening a command list: kicks off every async compile at once (parallel startup)
lib.ensure_all_initialized_no_materialize(*ctx);

// per frame, with a command list open: materialize whatever a reload invalidated
lib.ensure_all_initialized(*cmd);
```

`add_package` walks the transitive dependency closure, dedups packages by instance identity (so a
singleton reached twice is walked once), and flattens their routines into one list — dependencies
before the packages that depend on them. `ensure_all_*` then fans out over that list. The library
references the packages and keeps them alive; it does not own them (their `acquire()` static does).

### Hot reload

Reload tracking is entirely through slib's process-global generation, so the routine library needs no
wiring for it: the app sets up its `slib::shader_library` and calls `start_hot_reload()` as usual, and
when a shader reloads, `slib::current_reload_generation()` moves. Every routine reads that counter, so
the affected routines re-run `init_declare` + `init_materialize` on the next `ensure_*`. There is only
ever one live shader library, which is why a single global counter suffices.

## See also

- [shaders.md](../../shaped-graphics/docs/shaders.md) — the shader system the routines pull from.
- [concepts/caches.md](../../shaped-graphics/docs/concepts/caches.md) — `ctx.cached.acquire_*`, the
  layout/pipeline caches a routine's `init_declare` builds on.
- [cheat-sheet.md](../cheat-sheet.md) — the sr public API at a glance.
