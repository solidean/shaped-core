# shaped-rendering cheat sheet

Concrete render routines and helpers on top of shaped-graphics. Namespace `sr`. Depends on
shaped-graphics + shaped-shader-library. Headers are included by full path from `src/`:
`#include <shaped-rendering/<name>.hh>`.

> **Scope note:** the render-routine *framework* lives in **shaped-graphics** — `sg::render_routine`,
> `ctx.routines`, `sg::reload_generation` (see
> [shaped-graphics/cheat-sheet.md](../shaped-graphics/cheat-sheet.md) and
> [shaped-graphics/docs/render-routines.md](../shaped-graphics/docs/render-routines.md)). `sr` hosts
> the concrete routines — Dear ImGui today, mipmap gen / tonemapping later. Format conventions live in
> [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-rendering/all.hh>   // umbrella (window API + concrete routines as they land)
```

## Windows

Always available. Without a backend (SDL3 not fetched) `try_create` fails instead of the API disappearing;
`SR_HAS_WINDOW` (1/0) answers "is a backend compiled in" for the rare case you need it at compile time.

```cpp
#include <shaped-rendering/window.hh>

auto const wsys = sr::window_system::create({.headless = false});  // -> cc::unique_ptr<window_system>; try_create -> cc::result
auto const win  = wsys->create_window({.title = "viewer", .width = 1280, .height = 720,
                                       .is_resizable = true, .is_visible = true,
                                       .has_decoration = true, .is_always_on_top = false,
                                       .has_taskbar_icon = true, .is_focusable = true});  // -> cc::unique_ptr<window>

wsys->poll_events();              // drains the OS queue; refreshes every live window. Once per frame.
wsys->windows();                  // -> cc::span<window* const>, creation order, non-owning
wsys->is_quit_requested();        // latched OS-level quit; clear_quit_request()
wsys->is_headless();

win->native_window_handle();      // -> void* — HWND on Windows; feeds sg::swapchain_description
win->width();  win->height();     // -> int, pixels, as of the last poll_events
win->position();                  // -> tg::pos2i, desktop coords, may be negative on a multi-monitor desktop
win->set_position(tg::pos2i(x, y));  win->set_size(tg::vec2i(w, h));  // write-through: the getter reads it back at once
win->is_focused();                // -> bool, as of the last poll_events;  win->focus() asks for it
win->is_minimized();              // 0x0 while true
win->is_close_requested();        // latched; request_close() / clear_close_request()
win->title();  win->set_title(sv);
win->show();  win->hide();
win->system();                    // -> window_system& — the system it came from

wsys->set_cursor(sr::cursor_shape::text);   // arrow/text/wait/progress/crosshair/pointer/move/not_allowed/
wsys->cursor();                             //   resize_ns/resize_ew/resize_nesw/resize_nwse
wsys->set_cursor_visible(false);  wsys->is_cursor_visible();

wsys->clipboard_text();           // -> cc::string, empty if the clipboard holds no text
wsys->set_clipboard_text(sv);
wsys->has_clipboard_text();
```

- **Cursor and clipboard are process-global**, hence on the system rather than a window: one cursor shows at a
  time, whichever window the pointer is over.
- **`set_cursor` is cheap to call every frame** — the platform is only touched when the shape changes.
- **Hiding the cursor does not reset its shape**, so showing it again restores what was set.
- **Treat clipboard text as untrusted input** of unbounded size: it comes from any other application.

- **Poll once per frame, or the window hangs.**
  Nothing else advances a window's state, and an unpumped window is one the OS considers unresponsive.
- **Skip the frame while `is_minimized()`.**
  Size is 0x0 there, and `acquire_backbuffer`'s auto-resize would resize the chain to zero.
- **`window_system` is main-thread bound and at most one may be alive per process.** Both assert.
- **A window must not outlive its system**, and `~window_system` asserts if one does.
- **`native_window_handle()` is null off Windows and under a headless system** — nothing can present against those.

## Input

```cpp
#include <shaped-rendering/input.hh>   // pulled in by window.hh

for (auto const& e : wsys->events())   // -> cc::span<input_event const>, oldest first, all windows
{
    e.window;                          // -> sr::window*, null if none was focused
    if (auto const* k = std::get_if<sr::key_event>(&e.payload))
        k->scancode;    // sr::scancode — PHYSICAL position; WASD stays WASD on AZERTY
        k->character;   // char32_t — layout-mapped codepoint, 0 if unprintable; for ctrl+Z-style shortcuts
        k->modifiers;   // sr::key_modifiers bit set; has_all(k->modifiers, ctrl | shift)
        k->is_down;  k->is_repeat;
    if (auto const* t = std::get_if<sr::text_event>(&e.payload))  t->text;        // cc::string, UTF-8
    if (auto const* m = std::get_if<sr::mouse_move_event>(&e.payload))    m->cursor_pos, m->delta;  // pos2f, vec2f
    if (auto const* b = std::get_if<sr::mouse_button_event>(&e.payload))  b->button, b->is_down, b->cursor_pos;
    if (auto const* w = std::get_if<sr::mouse_wheel_event>(&e.payload))   w->delta;  // ticks, may be fractional
}

win->set_relative_mouse_mode(true);   // capture: cursor hidden, x/y meaningless, dx/dy unbounded (FPS camera)
win->start_text_input();              // begin text_events + IME for this window; stop_text_input() to end
```

- **`events()` is invalidated by the next `poll_events()`**, text included — copy anything you keep.
- **Physical vs character**: `scancode` is position, `character` is layout. Movement uses `scancode`, ctrl+Z uses `character`.
  `sr::scancode` is our own position vocabulary — its numeric values mean nothing outside sr.
- **Never rebuild text from `key_event`s** — an IME commits a whole phrase, a dead key commits nothing until the
  next keystroke, and a paste arrives as one `text_event`.
- **Text input is off until `start_text_input()`**, because while it is on the OS may swallow keystrokes to compose.
- **Wheel deltas are fractional** on trackpads; the platform's inverted-scroll flag is already applied.
- **Positions are `tg::pos2f`, motions `tg::vec2f`** — `pos - pos` gives the `vec` between them.
  tg has no `.x`/`.y`: index with `p[0]` / `p[1]`.
- **`input_event::payload` is `std::variant` only until `cc::variant` exists** — the alternatives are the API.

## Dear ImGui

Full doc: [docs/imgui.md](docs/imgui.md). Vendored docking branch; `<imgui.h>` is available to consumers.

```cpp
lib.add_package(sr::shader_package());       // once at startup, or routines acquire nothing

auto imgui = sr::imgui_context::create();    // owns ImGuiContext; docking on, viewports stubbed; move-only

imgui.begin_frame({.display_size = tg::vec2i(w, h), .delta_time = dt, .framebuffer_scale = {1, 1}});
ImGui::ShowDemoWindow();                     // any imgui calls
imgui.end_frame();                           // = ImGui::Render()

{
    auto pass = cmd->raster.render_to({.color_targets = {backbuffer.preserved()}});
    sr::imgui_routine::execute(*cmd, ImGui::GetDrawData(), {.target_format = fmt, .target_size = tg::vec2i(w, h)});
}

sr::imgui_routine::live_texture_count(ctx);           // -> isize, atlas textures alive (tests/diagnostics)
sr::imgui_routine::release_textures(ctx, draw_data);  // drop them; a later frame rebuilds
```

Gotchas:

- **`target_format` must not be sRGB** — imgui's colors are already sRGB-encoded; asserts rather than
  double-encoding. Bind a non-srgb view of the same resource.
- **One call, inside the scope.** Textures, geometry upload and draws all happen in `execute()`.
- **One thread.** The state is mutex-guarded, so two threads cannot corrupt it — but the routine is a
  per-context singleton holding *this frame's* geometry, so record imgui from one thread.

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
