---
created: 2026-10-17
refs:
  - file: extern/sdl3/CMakeLists.txt
  - file: .github/workflows/ci-linux-clang.yml
  - symbol: sr::window_system
---

# wayland is currently off in practice: SDL degrades to X11 without an error

[extern/sdl3](../../../../../extern/sdl3/CMakeLists.txt) forces `SDL_WAYLAND ON`.
But SDL's wayland backend pkg-checks for `wayland-client`, `wayland-egl`, `wayland-cursor`, `egl` and `xkbcommon`, and degrades to X11-only, without an error, if any is missing.
CI has no EGL development package, so `SDL_WAYLAND (Wanted: ON): OFF` in every Linux configure today.
The forced-but-degraded state is invisible unless someone reads SDL's configure summary.

Consequence on a developer machine: a Wayland session runs `sr::window` through XWayland rather than natively.

Two ways out:
- install `libegl-dev` (and probably `libwayland-bin` for `wayland-scanner`) so the forced option means what it says;
- stop forcing it, and document that wayland arrives with its dependencies.
