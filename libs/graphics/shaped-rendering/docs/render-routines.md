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

No concrete routines have landed yet — they arrive here as they are implemented, each with its own
tests. See [structure.md](structure.md) for the roadmap.
