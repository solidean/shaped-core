# shaped-rendering coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read
that first; everything there still applies. Because shaped-rendering is built directly on
shaped-graphics, its [coding-guidelines](../../shaped-graphics/docs/coding-guidelines.md) (handles,
resource model, backend model) are also relevant background.

This document is intentionally near-empty for now. **Extend it as we go:** whenever you catch
yourself making a "style mistake" by following generic advice that turns out to be wrong for sr
(for a reason that isn't obvious from the code), that's the signal to add the rule here.

## Only `impl/window_sdl.cc` sees SDL

`<SDL3/SDL.h>` appears in exactly one file.
No SDL type, macro, enum or forward declaration reaches a public header.
`window.hh` carries a `void*` where the implementation carries an `SDL_Window*`.

Three things fall out of that one seam, none of them obvious from the code:

* A consumer of sr inherits neither SDL's include cost nor its `main` macro.
  `SDL_main.h` `#define`s `main`, which would collide with nexus's.
* A checkout without SDL3 still compiles every other sr header.
  The window API is the only thing that disappears.
* Adding or replacing a windowing backend is one new `.cc` plus a CMake branch, with no public API movement.
  The same shape as shaped-shader-library's `impl/watch_backend_*.cc`.

## The window API is gated, and the gate is `SR_HAS_WINDOW`

SDL3 is fetched on demand, so sr must build without it.
CMake defines `SR_HAS_WINDOW` to `1` or `0` — always defined, never defined-vs-undefined.
`window.hh`, `fwd.hh` and `all.hh` all branch on `#if SR_HAS_WINDOW`.

Anything new that needs SDL goes inside that same `#if` and the same CMake branch.
Never add a second flag: two gates that can disagree is a build configuration nobody tests.
