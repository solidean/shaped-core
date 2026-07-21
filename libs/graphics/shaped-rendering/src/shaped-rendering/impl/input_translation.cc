#include <shaped-rendering/impl/input_translation.hh>

// Definitions for impl/input_translation.hh. Kept in their own translation unit so the mapping can be linked into
// a test without dragging in the window and event-pump machinery.

namespace sr::impl
{
sr::scancode scancode_from_sdl(SDL_Scancode sdl_scancode)
{
    switch (sdl_scancode)
    {
        // clang-format off
    case SDL_SCANCODE_A: return scancode::a;   case SDL_SCANCODE_B: return scancode::b;
    case SDL_SCANCODE_C: return scancode::c;   case SDL_SCANCODE_D: return scancode::d;
    case SDL_SCANCODE_E: return scancode::e;   case SDL_SCANCODE_F: return scancode::f;
    case SDL_SCANCODE_G: return scancode::g;   case SDL_SCANCODE_H: return scancode::h;
    case SDL_SCANCODE_I: return scancode::i;   case SDL_SCANCODE_J: return scancode::j;
    case SDL_SCANCODE_K: return scancode::k;   case SDL_SCANCODE_L: return scancode::l;
    case SDL_SCANCODE_M: return scancode::m;   case SDL_SCANCODE_N: return scancode::n;
    case SDL_SCANCODE_O: return scancode::o;   case SDL_SCANCODE_P: return scancode::p;
    case SDL_SCANCODE_Q: return scancode::q;   case SDL_SCANCODE_R: return scancode::r;
    case SDL_SCANCODE_S: return scancode::s;   case SDL_SCANCODE_T: return scancode::t;
    case SDL_SCANCODE_U: return scancode::u;   case SDL_SCANCODE_V: return scancode::v;
    case SDL_SCANCODE_W: return scancode::w;   case SDL_SCANCODE_X: return scancode::x;
    case SDL_SCANCODE_Y: return scancode::y;   case SDL_SCANCODE_Z: return scancode::z;

    case SDL_SCANCODE_0: return scancode::num_0;   case SDL_SCANCODE_1: return scancode::num_1;
    case SDL_SCANCODE_2: return scancode::num_2;   case SDL_SCANCODE_3: return scancode::num_3;
    case SDL_SCANCODE_4: return scancode::num_4;   case SDL_SCANCODE_5: return scancode::num_5;
    case SDL_SCANCODE_6: return scancode::num_6;   case SDL_SCANCODE_7: return scancode::num_7;
    case SDL_SCANCODE_8: return scancode::num_8;   case SDL_SCANCODE_9: return scancode::num_9;

    case SDL_SCANCODE_RETURN: return scancode::enter;
    case SDL_SCANCODE_ESCAPE: return scancode::escape;
    case SDL_SCANCODE_BACKSPACE: return scancode::backspace;
    case SDL_SCANCODE_TAB: return scancode::tab;
    case SDL_SCANCODE_SPACE: return scancode::space;
    case SDL_SCANCODE_INSERT: return scancode::insert;
    case SDL_SCANCODE_DELETE: return scancode::del;
    case SDL_SCANCODE_HOME: return scancode::home;
    case SDL_SCANCODE_END: return scancode::end;
    case SDL_SCANCODE_PAGEUP: return scancode::page_up;
    case SDL_SCANCODE_PAGEDOWN: return scancode::page_down;

    case SDL_SCANCODE_LEFT: return scancode::left;     case SDL_SCANCODE_RIGHT: return scancode::right;
    case SDL_SCANCODE_UP: return scancode::up;         case SDL_SCANCODE_DOWN: return scancode::down;

    case SDL_SCANCODE_MINUS: return scancode::minus;
    case SDL_SCANCODE_EQUALS: return scancode::equals;
    case SDL_SCANCODE_LEFTBRACKET: return scancode::left_bracket;
    case SDL_SCANCODE_RIGHTBRACKET: return scancode::right_bracket;
    case SDL_SCANCODE_BACKSLASH: return scancode::backslash;
    case SDL_SCANCODE_SEMICOLON: return scancode::semicolon;
    case SDL_SCANCODE_APOSTROPHE: return scancode::apostrophe;
    case SDL_SCANCODE_GRAVE: return scancode::grave;
    case SDL_SCANCODE_COMMA: return scancode::comma;
    case SDL_SCANCODE_PERIOD: return scancode::period;
    case SDL_SCANCODE_SLASH: return scancode::slash;

    case SDL_SCANCODE_LSHIFT: return scancode::left_shift;   case SDL_SCANCODE_RSHIFT: return scancode::right_shift;
    case SDL_SCANCODE_LCTRL: return scancode::left_ctrl;     case SDL_SCANCODE_RCTRL: return scancode::right_ctrl;
    case SDL_SCANCODE_LALT: return scancode::left_alt;       case SDL_SCANCODE_RALT: return scancode::right_alt;
    case SDL_SCANCODE_LGUI: return scancode::left_super;     case SDL_SCANCODE_RGUI: return scancode::right_super;

    case SDL_SCANCODE_CAPSLOCK: return scancode::caps_lock;
    case SDL_SCANCODE_NUMLOCKCLEAR: return scancode::num_lock;
    case SDL_SCANCODE_SCROLLLOCK: return scancode::scroll_lock;
    case SDL_SCANCODE_PRINTSCREEN: return scancode::print_screen;
    case SDL_SCANCODE_PAUSE: return scancode::pause;
    case SDL_SCANCODE_APPLICATION: return scancode::menu;

    case SDL_SCANCODE_F1: return scancode::f1;    case SDL_SCANCODE_F2: return scancode::f2;
    case SDL_SCANCODE_F3: return scancode::f3;    case SDL_SCANCODE_F4: return scancode::f4;
    case SDL_SCANCODE_F5: return scancode::f5;    case SDL_SCANCODE_F6: return scancode::f6;
    case SDL_SCANCODE_F7: return scancode::f7;    case SDL_SCANCODE_F8: return scancode::f8;
    case SDL_SCANCODE_F9: return scancode::f9;    case SDL_SCANCODE_F10: return scancode::f10;
    case SDL_SCANCODE_F11: return scancode::f11;  case SDL_SCANCODE_F12: return scancode::f12;

    case SDL_SCANCODE_KP_0: return scancode::kp_0;   case SDL_SCANCODE_KP_1: return scancode::kp_1;
    case SDL_SCANCODE_KP_2: return scancode::kp_2;   case SDL_SCANCODE_KP_3: return scancode::kp_3;
    case SDL_SCANCODE_KP_4: return scancode::kp_4;   case SDL_SCANCODE_KP_5: return scancode::kp_5;
    case SDL_SCANCODE_KP_6: return scancode::kp_6;   case SDL_SCANCODE_KP_7: return scancode::kp_7;
    case SDL_SCANCODE_KP_8: return scancode::kp_8;   case SDL_SCANCODE_KP_9: return scancode::kp_9;
    case SDL_SCANCODE_KP_DIVIDE: return scancode::kp_divide;
    case SDL_SCANCODE_KP_MULTIPLY: return scancode::kp_multiply;
    case SDL_SCANCODE_KP_MINUS: return scancode::kp_minus;
    case SDL_SCANCODE_KP_PLUS: return scancode::kp_plus;
    case SDL_SCANCODE_KP_ENTER: return scancode::kp_enter;
    case SDL_SCANCODE_KP_PERIOD: return scancode::kp_period;
        // clang-format on

    default:
        return scancode::unknown;
    }
}

key_modifiers modifiers_from_sdl(SDL_Keymod mod)
{
    auto result = key_modifiers::none;
    if (mod & SDL_KMOD_SHIFT)
        result |= key_modifiers::shift;
    if (mod & SDL_KMOD_CTRL)
        result |= key_modifiers::ctrl;
    if (mod & SDL_KMOD_ALT)
        result |= key_modifiers::alt;
    if (mod & SDL_KMOD_GUI)
        result |= key_modifiers::super;
    return result;
}

char32_t character_from_keycode(SDL_Keycode keycode)
{
    // SDL packs two kinds of value into one u32.
    // A key that produces a character *is* that character's codepoint — SDLK_A is 0x61, 'a'.
    // Every other key is its scancode with a flag bit set: SDLK_SCANCODE_MASK (1<<30) for named keys, so
    // SDLK_F1 is 0x4000003a, and SDLK_EXTENDED_MASK (1<<29) for extended ones.
    // Both flags land above Unicode's ceiling, so a range test separates the two without naming SDL's masks.
    //
    // This does admit the C0 controls SDL defines as codepoints — enter is U+000D, tab U+0009, escape U+001B,
    // backspace U+0008 — so a non-zero character is not the same as a printable one.
    constexpr auto unicode_max = SDL_Keycode(0x10FFFF);
    return keycode > 0 && keycode <= unicode_max ? char32_t(keycode) : char32_t(0);
}

sr::mouse_button mouse_button_from_sdl(u8 sdl_button)
{
    switch (sdl_button)
    {
    case SDL_BUTTON_LEFT:
        return mouse_button::left;
    case SDL_BUTTON_MIDDLE:
        return mouse_button::middle;
    case SDL_BUTTON_RIGHT:
        return mouse_button::right;
    case SDL_BUTTON_X1:
        return mouse_button::x1;
    default:
        return mouse_button::x2;
    }
}

float wheel_amount(float raw, SDL_MouseWheelDirection direction)
{
    return direction == SDL_MOUSEWHEEL_FLIPPED ? -raw : raw;
}
} // namespace sr::impl
