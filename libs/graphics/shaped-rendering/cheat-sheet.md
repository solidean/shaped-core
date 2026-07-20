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
#include <shaped-rendering/all.hh>   // umbrella (window API + concrete routines as they land)
```

## Windows

Present only when SDL3 was fetched — CMake defines `SR_HAS_WINDOW` to `1` or `0`.

```cpp
#include <shaped-rendering/window.hh>

auto const wsys = sr::window_system::create({.headless = false});  // -> cc::unique_ptr<window_system>; try_create -> cc::result
auto const win  = wsys->create_window({.title = "viewer", .width = 1280, .height = 720,
                                       .is_resizable = true, .is_visible = true});  // -> cc::unique_ptr<window>

wsys->poll_events();              // drains the OS queue; refreshes every live window. Once per frame.
wsys->windows();                  // -> cc::span<window* const>, creation order, non-owning
wsys->is_quit_requested();        // latched OS-level quit; clear_quit_request()
wsys->is_headless();

win->native_window_handle();      // -> void* — HWND on Windows; feeds sg::swapchain_description
win->width();  win->height();     // -> int, pixels, as of the last poll_events
win->is_minimized();              // 0x0 while true
win->is_close_requested();        // latched; request_close() / clear_close_request()
win->title();  win->set_title(sv);
win->show();  win->hide();
```

- **Poll once per frame, or the window hangs.**
  Nothing else advances a window's state, and an unpumped window is one the OS considers unresponsive.
- **Skip the frame while `is_minimized()`.**
  Size is 0x0 there, and `acquire_backbuffer`'s auto-resize would resize the chain to zero.
- **`window_system` is main-thread bound and at most one may be alive per process.** Both assert.
- **A window must not outlive its system**, and `~window_system` asserts if one does.
- **`native_window_handle()` is null off Windows and under a headless system** — nothing can present against those.

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
