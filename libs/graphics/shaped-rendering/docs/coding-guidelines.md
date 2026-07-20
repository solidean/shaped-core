# shaped-rendering coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read
that first; everything there still applies. Because shaped-rendering is built directly on
shaped-graphics, its [coding-guidelines](../../shaped-graphics/docs/coding-guidelines.md) (handles,
resource model, backend model) are also relevant background.

This document is intentionally near-empty for now. **Extend it as we go:** whenever you catch
yourself making a "style mistake" by following generic advice that turns out to be wrong for sr
(for a reason that isn't obvious from the code), that's the signal to add the rule here.

## SDL stays inside `impl/`

No SDL type, macro, enum or forward declaration reaches a **public** header.
`window.hh` carries a `void*` where the implementation carries an `SDL_Window*`.

SDL is visible only under `impl/` and in tests:

* `impl/window_sdl.cc` — the backend: init, windows, the event pump.
* `impl/input_translation.hh` / `.cc` — the pure SDL-to-sr mapping.
  A header rather than a detail of the backend, so it can be linked into a test; it names SDL types, which is why
  it lives under `impl/` and is absent from the public header set.
* `impl/window_internals.hh` — the backend's window id, so a test can address a window.
  Names no SDL type; it is internal because it exposes *how* the backend identifies windows.
* `tests/input-translation-test.cc` and `tests/input-dispatch-test.cc` — the two tests that include SDL.

The tests are a deliberate exception, not an erosion.
Both cover code that cannot be reached through the public API: a hundred-entry mapping table with two sign
conventions, and the event routing that decides which window a keystroke belongs to.
Untested, those are exactly where a wrong table entry or a misrouted event hides until a user finds it.
A test seeing SDL costs nothing outside sr's own build; a public header seeing it costs every consumer.

Extend the exception the same way — a test, or something internal under `impl/`, never a public header.
Reach for the narrowest hook that works: `backend_window_id` exists because the alternative was a test that
hardcoded the backend's property name, which would have duplicated a detail instead of sharing it.

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
