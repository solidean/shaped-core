#include <nexus/test.hh>
#include <shaped-rendering/impl/input_translation.hh>

// The SDL-to-sr input mapping, tested directly.
//
// This is the one test that reaches past sr's public API into impl/, and the one place besides the backend where
// SDL appears — see ../docs/coding-guidelines.md.
// The mapping is a hundred table entries and two sign conventions with no way to reach it through a window, so it
// is tested at the seam instead of not at all.
//
// Needs no display: every function here is pure.

using namespace sr;

TEST("sr - scancodes map to physical positions")
{
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_A) == scancode::a);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_Z) == scancode::z);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_0) == scancode::num_0);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_9) == scancode::num_9);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_ESCAPE) == scancode::escape);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_SPACE) == scancode::space);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_F1) == scancode::f1);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_F12) == scancode::f12);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_KP_0) == scancode::kp_0);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_KP_ENTER) == scancode::kp_enter);

    // SDL spells this one RETURN; sr spells it enter. An easy one to map to the keypad's by mistake.
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_RETURN) == scancode::enter);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_RETURN) != scancode::kp_enter);

    // Left and right instances are distinct positions, not one merged key.
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_LSHIFT) == scancode::left_shift);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_RSHIFT) == scancode::right_shift);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_LSHIFT) != impl::scancode_from_sdl(SDL_SCANCODE_RSHIFT));

    // A key sr does not model reads as unknown rather than as a neighbour.
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_F13) == scancode::unknown);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_INTERNATIONAL1) == scancode::unknown);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_UNKNOWN) == scancode::unknown);
}

TEST("sr - the WASD block keeps its layout-independent shape")
{
    // The reason sr exposes physical positions at all: these four must stay the inverted-T on any layout, which
    // holds because they are positions rather than the letters an AZERTY keyboard would produce here.
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_W) == scancode::w);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_A) == scancode::a);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_S) == scancode::s);
    CHECK(impl::scancode_from_sdl(SDL_SCANCODE_D) == scancode::d);
}

TEST("sr - modifiers map onto the bit set, merging left and right")
{
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_NONE) == key_modifiers::none);

    CHECK(impl::modifiers_from_sdl(SDL_KMOD_LSHIFT) == key_modifiers::shift);
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_RSHIFT) == key_modifiers::shift);
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_LCTRL) == key_modifiers::ctrl);
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_LALT) == key_modifiers::alt);
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_LGUI) == key_modifiers::super);

    auto const combined = impl::modifiers_from_sdl(SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_RSHIFT));
    CHECK(has_all(combined, key_modifiers::ctrl | key_modifiers::shift));
    CHECK(!has_all(combined, key_modifiers::alt));

    // Caps lock and num lock are states, not modifiers we report.
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_CAPS) == key_modifiers::none);
    CHECK(impl::modifiers_from_sdl(SDL_KMOD_NUM) == key_modifiers::none);
}

TEST("sr - printable keycodes become their codepoint")
{
    CHECK(impl::character_from_keycode(SDLK_A) == U'a');
    CHECK(impl::character_from_keycode(SDLK_Z) == U'z');
    CHECK(impl::character_from_keycode(SDLK_0) == U'0');
    CHECK(impl::character_from_keycode(SDLK_SPACE) == U' ');

    // Non-printable keys carry a bit above the Unicode range, and must not be mistaken for a character.
    CHECK(impl::character_from_keycode(SDLK_F1) == 0);
    CHECK(impl::character_from_keycode(SDLK_LEFT) == 0);
    CHECK(impl::character_from_keycode(SDLK_LSHIFT) == 0);
    CHECK(impl::character_from_keycode(SDLK_UNKNOWN) == 0);
}

TEST("sr - mouse buttons map, and anything unexpected lands on x2")
{
    CHECK(impl::mouse_button_from_sdl(SDL_BUTTON_LEFT) == mouse_button::left);
    CHECK(impl::mouse_button_from_sdl(SDL_BUTTON_MIDDLE) == mouse_button::middle);
    CHECK(impl::mouse_button_from_sdl(SDL_BUTTON_RIGHT) == mouse_button::right);
    CHECK(impl::mouse_button_from_sdl(SDL_BUTTON_X1) == mouse_button::x1);
    CHECK(impl::mouse_button_from_sdl(SDL_BUTTON_X2) == mouse_button::x2);

    // SDL numbers buttons from 1 and a fancy mouse can report more than five.
    CHECK(impl::mouse_button_from_sdl(9) == mouse_button::x2);
}

TEST("sr - wheel amounts follow the platform's scroll direction")
{
    CHECK(impl::wheel_amount(1.0f, SDL_MOUSEWHEEL_NORMAL) == 1.0f);
    CHECK(impl::wheel_amount(-2.5f, SDL_MOUSEWHEEL_NORMAL) == -2.5f);

    // Natural scrolling: SDL reports the raw direction plus a flag rather than pre-correcting it, so missing this
    // inverts scrolling for everyone who has it turned on.
    CHECK(impl::wheel_amount(1.0f, SDL_MOUSEWHEEL_FLIPPED) == -1.0f);
    CHECK(impl::wheel_amount(-2.5f, SDL_MOUSEWHEEL_FLIPPED) == 2.5f);

    CHECK(impl::wheel_amount(0.0f, SDL_MOUSEWHEEL_FLIPPED) == 0.0f);
}
