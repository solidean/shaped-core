# Render routines (in shaped-rendering)

shaped-rendering (`sr`) is the home for **concrete render routines** — the actual GPU algorithms
(mipmap generation, tonemapping, texture compression, …), each built on the render-routine framework.

The **framework itself lives in shaped-graphics** — `sg::render_routine`, the per-context
`ctx.routines` registry, and the `sg::reload_generation` hot-reload counter. Read its front-door doc
first:

- **[shaped-graphics/docs/render-routines.md](../../shaped-graphics/docs/render-routines.md)** — the
  routine base, three-phase init, by-type `acquire` / `prewarm` / `evict`, `ctx.routines.clear()`, and hot
  reload, end to end.

## Writing a concrete routine

A routine in `sr` derives from the CRTP base and lives in its own `.cc`/`.hh` pair:

```cpp
// libs/graphics/shaped-rendering/src/shaped-rendering/mipmap_routine.hh
class mipmap_routine : public sg::render_routine<mipmap_routine>
{
public:
    static void execute(sg::command_list& cmd, /* args */);   // acquire(cmd) + record the passes

protected:
    void init_declare(sg::context& ctx) override;             // acquire shaders (via slib) + pipelines
};
```

`sr` depends on **shaped-shader-library** because a concrete routine acquires its shaders through it in
`init_declare` — a routine-author dependency. The framework in `sg` needs no shader library; reload
tracking is `sg`'s own generation counter.

Register `sr::shader_package()` with your `slib::shader_library` once at startup, or every routine here
acquires nothing and draws nothing.

## A routine owns shader-derived state — nothing else

`init_declare` re-runs on every shader reload, so a routine's members are the things that *should* be
rebuilt then: layouts, pipelines, compiled shaders. Anything whose lifetime is driven from outside —
a resource pool, a per-frame allocation, a registry the caller mutates — does not belong in a routine,
and trying to put it there shows up immediately as a fight with `acquire()` returning a `const&`.

The worked example is Dear ImGui ([imgui.md](imgui.md)). ImGui owns the lifetime of its font atlas, so the
atlas lives on `sr::imgui_renderer`, an ordinary object the application holds, while
`sr::imgui_draw_routine` keeps only the layouts and pipelines and reads everything else out of its
`params`. That split is what let the routine stay `const` and the sg framework stay unchanged.

The one member the imgui routine does mutate is a `mutable` pipeline cache keyed by target format —
memoization of a pure function, and a stand-in for `ctx.cached.acquire_raster_pipeline` once
`pipeline_cache` grows a graphics tier.

Concrete routines arrive here as they are implemented, each with its own tests.
See [structure.md](structure.md) for the roadmap.
