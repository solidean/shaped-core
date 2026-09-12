# Render routines (in shaped-rendering)

shaped-rendering (`sr`) is the home for **concrete render routines** — the actual GPU algorithms, each built on the render-routine framework.
`sr::blit_routine` is the one that exists today; mipmap generation, tonemapping and texture compression are the intended set.

The **framework itself lives in shaped-graphics** — `sg::render_routine`, the per-context `ctx.routines` registry, and the `sg::reload_generation` hot-reload counter.
Read its front-door doc first:

- **[shaped-graphics/docs/render-routines.md](../../shaped-graphics/docs/render-routines.md)** — the routine base and its two coroutine phases, end to end.
  Also by-type `try_acquire` / `prewarm` / `evict`, the tick, dependency tokens, parametrized routines, and hot reload.

## Writing a concrete routine

A routine in `sr` derives from the CRTP base and lives in its own `.cc`/`.hh` pair:

```cpp
// libs/graphics/shaped-rendering/src/shaped-rendering/mipmap_routine.hh
class mipmap_routine : public sg::render_routine<mipmap_routine>
{
public:
    static void execute(sg::command_list& cmd, /* args */);   // try_acquire_exclusive(cmd), branch once, record

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;   // await shaders (via slib) + pipelines
};
```

`sr` depends on **shaped-shader-library** because a concrete routine acquires its shaders through it in `init` — a routine-author dependency.
The framework in `sg` needs no shader library; reload tracking is `sg`'s own generation counter.

Register `sr::shader_package()` with your `slib::shader_library` once at startup, or every routine here acquires nothing and draws nothing.

## A routine holds state — and the framework guards it

Routines are expected to keep things, and acquiring hands the same per-context instance to every caller.
So a routine that writes anything during `execute` opens with `try_acquire_exclusive(cmd)`, whose guard holds the routine's lock for its lifetime.
The members are then plain members — no `struct state`, no `cc::mutex` — see [shaped-graphics/docs/render-routines.md](../../shaped-graphics/docs/render-routines.md#threading).
What that lock does *not* cover is `init`: a coroutine cannot hold a `cc::mutex` across a `co_await`, so readiness is what publishes whatever a phase wrote.

The worked example is Dear ImGui ([imgui.md](imgui.md)).
`sr::imgui_routine` holds three kinds of state:
shader-derived (layouts, pipelines — rebuilt by `init` on every reload), imgui-owned (the font atlas textures — deliberately *not* rebuilt), and per-frame (the geometry).
The atlas lives on a routine of its own, reached through a dependency token, so every target format shares one instead of getting a copy each.
Sorting members by which of those they are is most of the design work — the first two live on the routine, and only the third does not.

Concrete routines arrive here as they are implemented, each with its own tests.
See [structure.md](structure.md) for the roadmap.
