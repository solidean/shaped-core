# shaped-rendering TODO

Running list of known follow-ups. Bigger design intent lives in [structure.md](structure.md).

- First routines on the framework: mipmap generation, texture compression, tonemapping.
- Get imgui off stb.
  It bundles stb rect-pack, truetype and textedit; we scope them with `IMGUI_STB_NAMESPACE` so they cannot collide with anyone else's stb, but scoping is containment, not a fix.
  stb is hobby-grade — no release process, known robustness gaps parsing malformed fonts — and it sits on the path that loads user-supplied font files.
  Either upstream's freetype backend (`misc/freetype`, a second vendored dep) or our own rasterizer.
- imgui viewports, **upstream bug, not ours**:
  while a detached viewport window is dragged over another, the dock-target markers drawn inside it lag the window, snapping back into place the moment it stops moving.
  Reproduced with **stock imgui** — upstream `example_win32_directx12` built from the tag we vendor (`v1.92.8-docking`, `b61e5634`) with upstream's own win32 + dx12 backends and none of our code —
  so nothing in `sr` causes it and nothing in `sr` can fix it.
  Matches [ocornut/imgui#7664](https://github.com/ocornut/imgui/issues/7664) ("docking markers move with window"), reported there against glfw + vulkan on X11: two unrelated backend stacks, same symptom.
  Unresolved upstream, no maintainer response.
  Ruled out along the way, each by measurement rather than argument, so nobody repeats it:
  the input translation to desktop space,
  the coordinates handed to the routine (window position equals `ImDrawData::DisplayPos` at submit time, sampled from `GetWindowRect`),
  the routine's own handling of a non-zero `DisplayPos` (now pinned by `sr::imgui_routine - a non-zero display pos shifts what lands on the target`),
  and the order of viewport update against the main window's present.
- imgui: `ImDrawCmd::UserCallback` is skipped; supporting it also means `ImDrawCallback_ResetRenderState`.
- imgui viewports: `PlatformRequestMove` / `PlatformRequestResize` are unset, so imgui is not told when the *window manager* rather than imgui moves or resizes a viewport window.
  It cannot be detected by comparing positions —
  `window::position` holds what was last written through while the platform reports what its event queue last confirmed, and those differ during any motion for reasons unrelated to the window manager.
  It needs the event, which means a moved/resized latch on `sr::window` set in `poll_events`.
- imgui viewports: `Platform_SetWindowAlpha` is not implemented, so imgui cannot fade the drag payload window.
- imgui viewports: a viewport swapchain is pinned to `bgra8_unorm` rather than matching the caller's main swapchain, which the routine never learns.
  Worth threading through once a caller wants a different format.
- imgui viewports: `ImGuiBackendFlags_HasMouseHoveredViewport` is not set, so imgui infers the hovered viewport from the mouse position rather than asking the platform.
  That is wrong when another application's window sits on top of a viewport — SDL would have to report the window under the cursor.
- imgui: an Alpha8 atlas path (the shader samples `.rgba`, so `TexDesiredFormat` is pinned to RGBA32).
- `imgui_routine` keeps its own pipeline-per-format map; fold it into `ctx.cached.acquire_raster_pipeline` once `pipeline_cache` grows a graphics tier.
- Concrete routines currently need dx12 + DXC (Windows) for a real shader; the framework itself is cross-platform.
  Broaden once a non-Windows shader path exists.
- The routine library watches `std::shared_ptr<slib::shader_library>`; `add_shader_library` accepts any number, though slib currently allows one alive at a time.
- Settle the module layout once the first routines land, and grow the [cheat-sheet](../cheat-sheet.md) + [structure](structure.md) accordingly.

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
  A single SDL build can carry both and picks per session (`SDL_GetCurrentVideoDriver`), so the tag has to be a runtime discriminant that sg switches on — not a `#if` the way Windows and macOS could be.
  SDL already exposes the pieces as window properties (`SDL_PROP_WINDOW_X11_DISPLAY_POINTER` / `..._X11_WINDOW_NUMBER`, `..._WAYLAND_DISPLAY_POINTER` / `..._WAYLAND_SURFACE_POINTER`).

- **wayland is currently off in practice.**
  [extern/sdl3](../../../../extern/sdl3/CMakeLists.txt) forces `SDL_WAYLAND ON`, but SDL's wayland backend pkg-checks for `wayland-client`, `wayland-egl`, `wayland-cursor`, `egl` and `xkbcommon` —
  and degrades to X11-only, without an error, if any is missing.
  CI has no EGL development package, so `SDL_WAYLAND (Wanted: ON): OFF` in every Linux configure today.
  Consequence on a developer machine: a Wayland session runs `sr::window` through XWayland rather than natively.
  Either install `libegl-dev` (and probably `libwayland-bin` for `wayland-scanner`) so the forced option means what it says, or stop forcing it and document that wayland arrives with its dependencies.
  Worth noting the forced-but-degraded state is invisible unless someone reads SDL's configure summary.
- `SDL_JOYSTICK` is off in [extern/sdl3/CMakeLists.txt](../../../../extern/sdl3/CMakeLists.txt).
  imgui gamepad navigation will want it — a one-line flip, kept off today so Linux needs no libudev/evdev.
- HiDPI: sizes are queried in pixels, but `SDL_WINDOW_HIGH_PIXEL_DENSITY` is not requested, so logical and pixel sizes are identical.
  Revisit with a per-monitor-DPI UI.
- `input_event::payload` is a `std::variant` because [`cc::variant`](../../../base/clean-core/src/clean-core/container/variant.hh) is a declared-but-empty stub.
  Switch it over when clean-core grows a real one — the alternatives are the API, the holder is not.
- IME composition is not covered: a synthetic `SDL_EVENT_TEXT_INPUT` exercises delivery and the copy, but a real round trip through a platform input method needs an IME and a person.
- Gamepad input needs `SDL_JOYSTICK`, which is off in [extern/sdl3/CMakeLists.txt](../../../../extern/sdl3/CMakeLists.txt).
- `sr::scancode` deliberately stops at F12 and skips the media, `AC_*` and `INTERNATIONAL*` keys; they read as `scancode::unknown`.
- The hand-rolled Win32 windows in shaped-graphics' dx12 swapchain test and the two shaped-shader-compiler-dxc cube tests **cannot** be replaced by `sr::window`.
  Both libraries sit *below* sr, so consuming it would invert the dependency graph.
  The duplication is structural, not an oversight — a windowed test above sr belongs in shaped-viewer.
