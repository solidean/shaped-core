---
created: 2026-10-17
refs:
  - file: extern/sdl3/CMakeLists.txt
  - symbol: sr::input_event
---

# Gamepad input in `sr::window_system`

Gamepad input needs `SDL_JOYSTICK`, which is off in [extern/sdl3/CMakeLists.txt](../../../../../extern/sdl3/CMakeLists.txt) (and haptic with it, since SDL refuses haptic without joystick).
imgui gamepad navigation will want it.
The SDL side is a one-line flip, kept off today so Linux needs no libudev/evdev.
`sr::input_event` then needs a gamepad payload.
