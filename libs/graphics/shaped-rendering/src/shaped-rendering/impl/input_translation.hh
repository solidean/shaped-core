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

/// SDL's held-button mask as `mouse_button_bit`s.
[[nodiscard]] u8 held_buttons_from_sdl(SDL_MouseButtonFlags state);

/// The scroll amount with the platform's inverted-scroll flag applied.
/// SDL reports the raw direction plus a flag rather than pre-correcting it, so forgetting this inverts scrolling
/// for every user who has natural scrolling on.
[[nodiscard]] float wheel_amount(float raw, SDL_MouseWheelDirection direction);

/// SDL's pixel density with its failure value replaced: SDL_GetWindowPixelDensity returns 0 on error, and a 0 here would collapse every position onto the origin.
[[nodiscard]] float pixel_density_from_sdl(float density);

/// An SDL event coordinate, which is in logical units, in the pixels sr reports input in.
/// `density` is the window's pixels per logical unit, so a Retina window's (10, 20) is (20, 40).
[[nodiscard]] tg::pos2f pixels_from_logical(float x, float y, float density);
[[nodiscard]] tg::vec2f pixels_from_logical_delta(float dx, float dy, float density);
} // namespace sr::impl
