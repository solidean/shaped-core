#pragma once

#include <SDL3/SDL.h>
#include <shaped-rendering/input.hh>

/// The SDL-to-sr input mapping, split out from window_sdl.cc so a test can drive it directly.
///
/// Internal: this header names SDL types, so it lives under impl/ and is not part of the public header set.
/// Nothing outside shaped-rendering's own sources and tests may include it — see docs/coding-guidelines.md.
///
/// These are pure functions of their arguments, which is the whole point.
/// The mapping is the part most likely to be wrong (a hundred table entries, two sign conventions) and the part
/// least able to be reached through the public API, so it is tested here rather than through a live window.

namespace sr::impl
{
/// The physical position an SDL scancode names, or scancode::unknown for one sr does not model.
/// Deliberately partial: the media, AC_*, INTERNATIONAL* and F13+ keys have no sr::scancode, and a key sr does not
/// model must read as unknown rather than as some neighbouring key.
[[nodiscard]] sr::scancode scancode_from_sdl(SDL_Scancode sdl_scancode);

[[nodiscard]] key_modifiers modifiers_from_sdl(SDL_Keymod mod);

/// The character an SDL keycode produces, or 0 when it produces none.
/// A printable SDL_Keycode is its own Unicode codepoint; everything else has a bit above the Unicode range set.
[[nodiscard]] char32_t character_from_keycode(SDL_Keycode keycode);

/// SDL numbers its mouse buttons from 1; anything past the two extra buttons is reported as x2.
[[nodiscard]] sr::mouse_button mouse_button_from_sdl(u8 sdl_button);

/// The scroll amount with the platform's inverted-scroll flag applied.
/// SDL reports the raw direction plus a flag rather than pre-correcting it, so forgetting this inverts scrolling
/// for every user who has natural scrolling on.
[[nodiscard]] float wheel_amount(float raw, SDL_MouseWheelDirection direction);
} // namespace sr::impl
