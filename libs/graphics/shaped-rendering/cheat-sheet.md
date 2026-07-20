# shaped-rendering cheat sheet

Concrete render routines and helpers on top of shaped-graphics. Namespace `sr`. Depends on
shaped-graphics + shaped-shader-library. Headers are included by full path from `src/`:
`#include <shaped-rendering/<name>.hh>`.

> **Scope note:** the render-routine *framework* lives in **shaped-graphics** — `sg::render_routine`,
> `ctx.routines`, `sg::reload_generation` (see
> [shaped-graphics/cheat-sheet.md](../shaped-graphics/cheat-sheet.md) and
> [shaped-graphics/docs/render-routines.md](../shaped-graphics/docs/render-routines.md)). `sr` hosts
> the concrete routines (mipmap gen, tonemapping, …), which land later. Format conventions live in
> [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-rendering/all.hh>   // umbrella (fwd only for now — concrete routines land here later)
```

## Writing a concrete routine

```cpp
#include <shaped-graphics/render_routine.hh>
class my_routine : public sg::render_routine<my_routine>   // CRTP base; override the phases you need
{
public:
    static void execute(sg::command_list& cmd, /* args */)  // acquire(cmd) + record work
    { auto const& self = acquire(cmd); /* ... */ }
protected:
    void init_declare(sg::context& ctx) override { /* acquire shaders (slib) + pipelines */ }
};
// call site: my_routine::execute(cmd, args);   // reached by type — no handle, no registration
```

See the [shaped-graphics cheat sheet](../shaped-graphics/cheat-sheet.md) for the full framework surface
(`acquire` / `prewarm` / `evict`, the three init phases, `ctx.routines.clear()`).
