# shaped-rendering TODO

Running list of known follow-ups. Bigger design intent lives in [structure.md](structure.md).

- First routines on the framework: mipmap generation, texture compression, tonemapping.
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
