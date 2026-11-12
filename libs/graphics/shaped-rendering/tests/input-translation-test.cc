#include <nexus/test.hh>
#include <shaped-rendering/impl/input_translation.hh>
#include <shaped-rendering/window.hh>

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

    // SDL spells this one RETURN; sr spells it enter.
    // An easy one to map to the keypad's by mistake.
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

    // SDL defines these as the C0 controls they stand for, and they are inside the Unicode range, so they come through as characters.
    // A non-zero character is therefore not the same as a printable one.
    CHECK(impl::character_from_keycode(SDLK_RETURN) == U'\r');
    CHECK(impl::character_from_keycode(SDLK_TAB) == U'\t');
    CHECK(impl::character_from_keycode(SDLK_BACKSPACE) == U'\b');
    CHECK(impl::character_from_keycode(SDLK_ESCAPE) == char32_t(0x1B));
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

TEST("sr - mouse coordinates convert from logical units to pixels")
{
    // A Retina window: SDL's logical (10, 20) is pixel (20, 40), and a delta scales the same way.
    CHECK(impl::pixels_from_logical(10.0f, 20.0f, 2.0f) == tg::pos2f(20.0f, 40.0f));
    CHECK(impl::pixels_from_logical_delta(-1.5f, 3.0f, 2.0f) == tg::vec2f(-3.0f, 6.0f));

    // A fractionally scaled output keeps sub-pixel positions rather than rounding them.
    CHECK(impl::pixels_from_logical(3.0f, 5.0f, 1.5f) == tg::pos2f(4.5f, 7.5f));

    CHECK(impl::pixels_from_logical(7.0f, 9.0f, 1.0f) == tg::pos2f(7.0f, 9.0f));
}

TEST("sr - a failed pixel density query reads as 1")
{
    // SDL_GetWindowPixelDensity returns 0 on error, which would collapse every mouse position onto the origin.
    CHECK(impl::pixel_density_from_sdl(0.0f) == 1.0f);
    CHECK(impl::pixel_density_from_sdl(2.0f) == 2.0f);
    CHECK(impl::pixel_density_from_sdl(1.5f) == 1.5f);
}

TEST("sr - gamepad buttons map by position, and what sr does not model reads as none")
{
    CHECK(impl::gamepad_button_from_sdl(SDL_GAMEPAD_BUTTON_SOUTH) == gamepad_button::south);
    CHECK(impl::gamepad_button_from_sdl(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) == gamepad_button::dpad_right);
    CHECK(!impl::gamepad_button_from_sdl(SDL_GAMEPAD_BUTTON_TOUCHPAD).has_value());
    CHECK(!impl::gamepad_button_from_sdl(SDL_GAMEPAD_BUTTON_MISC1).has_value());
}

TEST("sr - a stick reads up as positive, and reaches its end on both sides")
{
    // SDL's y points down, and its range is one longer below zero than above.
    CHECK(impl::gamepad_axis_value(gamepad_axis::left_y, -32768) == 1.0f);
    CHECK(impl::gamepad_axis_value(gamepad_axis::left_y, 32767) == -1.0f);
    CHECK(impl::gamepad_axis_value(gamepad_axis::right_y, -32768) == 1.0f);

    CHECK(impl::gamepad_axis_value(gamepad_axis::left_x, -32768) == -1.0f);
    CHECK(impl::gamepad_axis_value(gamepad_axis::left_x, 32767) == 1.0f);
    CHECK(impl::gamepad_axis_value(gamepad_axis::left_x, 0) == 0.0f);
}

TEST("sr - a trigger reads 0 to 1")
{
    CHECK(impl::gamepad_axis_value(gamepad_axis::left_trigger, 0) == 0.0f);
    CHECK(impl::gamepad_axis_value(gamepad_axis::right_trigger, 32767) == 1.0f);
}

#if SR_HAS_WINDOW

namespace
{
[[nodiscard]] SDL_JoystickID attach_virtual_gamepad()
{
    auto desc = SDL_VirtualJoystickDesc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
    desc.name = "virtual pad";
    return SDL_AttachVirtualJoystick(&desc);
}
} // namespace

// SDL's virtual joystick goes through the same path a real pad's input does, from the driver up.
TEST("sr - a gamepad connects, reports its input as events and state, and disconnects",
     main_thread,
     exclusive("sr-window-system"))
{
    auto const wsys = sr::window_system::create({.headless = true});
    CHECK(wsys->gamepads().empty());

    auto const id = attach_virtual_gamepad();
    REQUIRE(id != 0);
    wsys->poll_events();
    REQUIRE(wsys->gamepads().size() == 1);
    CHECK(wsys->gamepads()[0].gamepad == id);
    CHECK(cc::string_view(wsys->gamepads()[0].name) == "virtual pad");
    REQUIRE(wsys->events().size() == 1);
    CHECK(wsys->events()[0].window == nullptr);
    CHECK(wsys->events()[0].as_gamepad_connection().is_connected);

    auto* const joystick = SDL_GetJoystickFromID(id);
    REQUIRE(joystick != nullptr);
    REQUIRE(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, -32768));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767));
    SDL_UpdateJoysticks();
    wsys->poll_events();

    auto const& pad = wsys->gamepads()[0];
    CHECK(pad.is_held(gamepad_button::south));
    CHECK(!pad.is_held(gamepad_button::east));
    CHECK(pad.left_stick[1] == 1.0f);
    CHECK(pad.right_trigger == 1.0f);

    // SDL also reports every axis's value on the first update, so the moved one is looked for rather than counted.
    auto buttons = 0;
    auto saw_stick = false;
    for (auto const& e : wsys->events())
    {
        buttons += e.is_gamepad_button() ? 1 : 0;
        if (e.is_gamepad_axis() && e.as_gamepad_axis().axis == gamepad_axis::left_y)
            saw_stick = saw_stick || e.as_gamepad_axis().value == 1.0f;
    }
    CHECK(buttons == 1);
    CHECK(saw_stick);

    REQUIRE(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false));
    SDL_UpdateJoysticks();
    wsys->poll_events();
    CHECK(!wsys->gamepads()[0].is_held(gamepad_button::south));

    REQUIRE(SDL_DetachVirtualJoystick(id));
    wsys->poll_events();
    CHECK(wsys->gamepads().empty());
    auto disconnected = 0;
    for (auto const& e : wsys->events())
        disconnected += e.is_gamepad_connection() && !e.as_gamepad_connection().is_connected ? 1 : 0;
    CHECK(disconnected == 1);
}

#endif
