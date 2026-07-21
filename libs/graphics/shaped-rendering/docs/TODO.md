# shaped-rendering TODO

Running list of known follow-ups. Bigger design intent lives in [structure.md](structure.md).

- First routines on the framework: mipmap generation, texture compression, tonemapping.
- imgui: wire the platform half once the windowing system lands — input events, cursors, clipboard
  (see the `TODO(windowing)` block in `imgui_context.cc`).
- imgui: multi-viewport (`ImGuiConfigFlags_ViewportsEnable`) needs per-viewport OS windows + swapchains.
- imgui: `ImDrawCmd::UserCallback` is skipped; supporting it also means `ImDrawCallback_ResetRenderState`.
- imgui: an Alpha8 atlas path (the shader samples `.rgba`, so `TexDesiredFormat` is pinned to RGBA32).
- `imgui_draw_routine` keeps its own pipeline-per-format map; fold it into
  `ctx.cached.acquire_raster_pipeline` once `pipeline_cache` grows a graphics tier.
- Concrete routines currently need dx12 + DXC (Windows) for a real shader; the framework itself is
  cross-platform. Broaden once a non-Windows shader path exists.
- The routine library watches `std::shared_ptr<slib::shader_library>`; `add_shader_library` accepts any
  number, though slib currently allows one alive at a time.
- Settle the module layout once the first routines land, and grow the
  [cheat-sheet](../cheat-sheet.md) + [structure](structure.md) accordingly.

Windowing:

- `native_window_handle()` is a bare `void*` and null off Windows.
  sg defers a platform-tagged handle struct until a second *consumer* exists (see its [TODO](../../shaped-graphics/docs/TODO.md)).
  The vulkan swapchain landing is that moment, and gives the struct two real callers to be designed against.

  What a `void*` can carry decides where presentation is possible at all:

  | Platform | Needs | Fits one pointer |
  |----------|-------|------------------|
  | Windows  | `HWND` | yes |
  | macOS    | `NSWindow*` | yes |
  | X11      | `Display*` + `Window` | no |
  | wayland  | `wl_display*` + `wl_surface*` | no |

  So **Linux presentation needs the tagged struct whether or not wayland is in the picture** — X11 already needs two values.

  The part that shapes the design: on Linux, X11-vs-wayland is a **runtime** choice, not a build-time one.
  A single SDL build can carry both and picks per session (`SDL_GetCurrentVideoDriver`), so the tag has to be a runtime
  discriminant that sg switches on — not a `#if` the way Windows and macOS could be.
  SDL already exposes the pieces as window properties (`SDL_PROP_WINDOW_X11_DISPLAY_POINTER` /
  `..._X11_WINDOW_NUMBER`, `..._WAYLAND_DISPLAY_POINTER` / `..._WAYLAND_SURFACE_POINTER`).

- **wayland is currently off in practice.** [extern/sdl3](../../../../extern/sdl3/CMakeLists.txt) forces `SDL_WAYLAND ON`,
  but SDL's wayland backend pkg-checks for `wayland-client`, `wayland-egl`, `wayland-cursor`, `egl` and `xkbcommon` —
  and degrades to X11-only, without an error, if any is missing.
  CI has no EGL development package, so `SDL_WAYLAND (Wanted: ON): OFF` in every Linux configure today.
  Consequence on a developer machine: a Wayland session runs `sr::window` through XWayland rather than natively.
  Either install `libegl-dev` (and probably `libwayland-bin` for `wayland-scanner`) so the forced option means what it
  says, or stop forcing it and document that wayland arrives with its dependencies.
  Worth noting the forced-but-degraded state is invisible unless someone reads SDL's configure summary.
- `SDL_JOYSTICK` is off in [extern/sdl3/CMakeLists.txt](../../../../extern/sdl3/CMakeLists.txt).
  imgui gamepad navigation will want it — a one-line flip, kept off today so Linux needs no libudev/evdev.
- HiDPI: sizes are queried in pixels, but `SDL_WINDOW_HIGH_PIXEL_DENSITY` is not requested, so logical and pixel sizes are identical.
  Revisit with a per-monitor-DPI UI.
- `input_event::payload` is a `std::variant` because [`cc::variant`](../../../base/clean-core/src/clean-core/container/variant.hh) is a declared-but-empty stub.
  Switch it over when clean-core grows a real one — the alternatives are the API, the holder is not.
- IME composition is not covered: a synthetic `SDL_EVENT_TEXT_INPUT` exercises delivery and the copy, but a real
  round trip through a platform input method needs an IME and a person.
- Gamepad input needs `SDL_JOYSTICK`, which is off in [extern/sdl3/CMakeLists.txt](../../../../extern/sdl3/CMakeLists.txt).
- `sr::scancode` deliberately stops at F12 and skips the media, `AC_*` and `INTERNATIONAL*` keys; they read as `scancode::unknown`.
- The hand-rolled Win32 windows in shaped-graphics' dx12 swapchain test and the two shaped-shader-compiler-dxc cube tests **cannot** be replaced by `sr::window`.
  Both libraries sit *below* sr, so consuming it would invert the dependency graph.
  The duplication is structural, not an oversight — a windowed test above sr belongs in shaped-viewer.
