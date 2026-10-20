---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/tests/window-manual-test.cc
  - file: libs/graphics/shaped-rendering/tests/input-dispatch-test.cc
  - symbol: sr::text_event
---

# IME composition is not covered by any test

A synthetic `SDL_EVENT_TEXT_INPUT` exercises delivery and the copy.
A real round trip through a platform input method needs an IME and a person, so it can only be a manual test.
