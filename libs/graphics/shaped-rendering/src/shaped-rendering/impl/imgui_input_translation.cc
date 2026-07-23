#include <shaped-rendering/impl/imgui_input_translation.hh>
#include <shaped-rendering/window.hh>

namespace sr::impl
{
ImGuiKey imgui_key_from_scancode(sr::scancode code)
{
    switch (code)
    {
    case scancode::a:
        return ImGuiKey_A;
    case scancode::b:
        return ImGuiKey_B;
    case scancode::c:
        return ImGuiKey_C;
    case scancode::d:
        return ImGuiKey_D;
    case scancode::e:
        return ImGuiKey_E;
    case scancode::f:
        return ImGuiKey_F;
    case scancode::g:
        return ImGuiKey_G;
    case scancode::h:
        return ImGuiKey_H;
    case scancode::i:
        return ImGuiKey_I;
    case scancode::j:
        return ImGuiKey_J;
    case scancode::k:
        return ImGuiKey_K;
    case scancode::l:
        return ImGuiKey_L;
    case scancode::m:
        return ImGuiKey_M;
    case scancode::n:
        return ImGuiKey_N;
    case scancode::o:
        return ImGuiKey_O;
    case scancode::p:
        return ImGuiKey_P;
    case scancode::q:
        return ImGuiKey_Q;
    case scancode::r:
        return ImGuiKey_R;
    case scancode::s:
        return ImGuiKey_S;
    case scancode::t:
        return ImGuiKey_T;
    case scancode::u:
        return ImGuiKey_U;
    case scancode::v:
        return ImGuiKey_V;
    case scancode::w:
        return ImGuiKey_W;
    case scancode::x:
        return ImGuiKey_X;
    case scancode::y:
        return ImGuiKey_Y;
    case scancode::z:
        return ImGuiKey_Z;

    case scancode::num_0:
        return ImGuiKey_0;
    case scancode::num_1:
        return ImGuiKey_1;
    case scancode::num_2:
        return ImGuiKey_2;
    case scancode::num_3:
        return ImGuiKey_3;
    case scancode::num_4:
        return ImGuiKey_4;
    case scancode::num_5:
        return ImGuiKey_5;
    case scancode::num_6:
        return ImGuiKey_6;
    case scancode::num_7:
        return ImGuiKey_7;
    case scancode::num_8:
        return ImGuiKey_8;
    case scancode::num_9:
        return ImGuiKey_9;

    case scancode::enter:
        return ImGuiKey_Enter;
    case scancode::escape:
        return ImGuiKey_Escape;
    case scancode::backspace:
        return ImGuiKey_Backspace;
    case scancode::tab:
        return ImGuiKey_Tab;
    case scancode::space:
        return ImGuiKey_Space;
    case scancode::insert:
        return ImGuiKey_Insert;
    case scancode::del:
        return ImGuiKey_Delete;
    case scancode::home:
        return ImGuiKey_Home;
    case scancode::end:
        return ImGuiKey_End;
    case scancode::page_up:
        return ImGuiKey_PageUp;
    case scancode::page_down:
        return ImGuiKey_PageDown;

    case scancode::left:
        return ImGuiKey_LeftArrow;
    case scancode::right:
        return ImGuiKey_RightArrow;
    case scancode::up:
        return ImGuiKey_UpArrow;
    case scancode::down:
        return ImGuiKey_DownArrow;

    case scancode::minus:
        return ImGuiKey_Minus;
    case scancode::equals:
        return ImGuiKey_Equal;
    case scancode::left_bracket:
        return ImGuiKey_LeftBracket;
    case scancode::right_bracket:
        return ImGuiKey_RightBracket;
    case scancode::backslash:
        return ImGuiKey_Backslash;
    case scancode::semicolon:
        return ImGuiKey_Semicolon;
    case scancode::apostrophe:
        return ImGuiKey_Apostrophe;
    case scancode::grave:
        return ImGuiKey_GraveAccent;
    case scancode::comma:
        return ImGuiKey_Comma;
    case scancode::period:
        return ImGuiKey_Period;
    case scancode::slash:
        return ImGuiKey_Slash;

    case scancode::left_shift:
        return ImGuiKey_LeftShift;
    case scancode::right_shift:
        return ImGuiKey_RightShift;
    case scancode::left_ctrl:
        return ImGuiKey_LeftCtrl;
    case scancode::right_ctrl:
        return ImGuiKey_RightCtrl;
    case scancode::left_alt:
        return ImGuiKey_LeftAlt;
    case scancode::right_alt:
        return ImGuiKey_RightAlt;
    case scancode::left_super:
        return ImGuiKey_LeftSuper;
    case scancode::right_super:
        return ImGuiKey_RightSuper;

    case scancode::caps_lock:
        return ImGuiKey_CapsLock;
    case scancode::num_lock:
        return ImGuiKey_NumLock;
    case scancode::scroll_lock:
        return ImGuiKey_ScrollLock;
    case scancode::print_screen:
        return ImGuiKey_PrintScreen;
    case scancode::pause:
        return ImGuiKey_Pause;
    case scancode::menu:
        return ImGuiKey_Menu;

    case scancode::f1:
        return ImGuiKey_F1;
    case scancode::f2:
        return ImGuiKey_F2;
    case scancode::f3:
        return ImGuiKey_F3;
    case scancode::f4:
        return ImGuiKey_F4;
    case scancode::f5:
        return ImGuiKey_F5;
    case scancode::f6:
        return ImGuiKey_F6;
    case scancode::f7:
        return ImGuiKey_F7;
    case scancode::f8:
        return ImGuiKey_F8;
    case scancode::f9:
        return ImGuiKey_F9;
    case scancode::f10:
        return ImGuiKey_F10;
    case scancode::f11:
        return ImGuiKey_F11;
    case scancode::f12:
        return ImGuiKey_F12;

    case scancode::kp_0:
        return ImGuiKey_Keypad0;
    case scancode::kp_1:
        return ImGuiKey_Keypad1;
    case scancode::kp_2:
        return ImGuiKey_Keypad2;
    case scancode::kp_3:
        return ImGuiKey_Keypad3;
    case scancode::kp_4:
        return ImGuiKey_Keypad4;
    case scancode::kp_5:
        return ImGuiKey_Keypad5;
    case scancode::kp_6:
        return ImGuiKey_Keypad6;
    case scancode::kp_7:
        return ImGuiKey_Keypad7;
    case scancode::kp_8:
        return ImGuiKey_Keypad8;
    case scancode::kp_9:
        return ImGuiKey_Keypad9;
    case scancode::kp_divide:
        return ImGuiKey_KeypadDivide;
    case scancode::kp_multiply:
        return ImGuiKey_KeypadMultiply;
    case scancode::kp_minus:
        return ImGuiKey_KeypadSubtract;
    case scancode::kp_plus:
        return ImGuiKey_KeypadAdd;
    case scancode::kp_enter:
        return ImGuiKey_KeypadEnter;
    case scancode::kp_period:
        return ImGuiKey_KeypadDecimal;

    case scancode::unknown:
        break;
    }
    return ImGuiKey_None;
}

int imgui_mouse_button_from(sr::mouse_button button)
{
    switch (button)
    {
    case mouse_button::left:
        return 0;
    case mouse_button::right:
        return 1; // not 2 — imgui orders right before middle
    case mouse_button::middle:
        return 2;
    case mouse_button::x1:
        return 3;
    case mouse_button::x2:
        return 4;
    }
    return 0;
}

sr::cursor_shape cursor_shape_from_imgui(int imgui_cursor)
{
    switch (imgui_cursor)
    {
    case ImGuiMouseCursor_TextInput:
        return cursor_shape::text;
    case ImGuiMouseCursor_ResizeAll:
        return cursor_shape::move;
    case ImGuiMouseCursor_ResizeNS:
        return cursor_shape::resize_ns;
    case ImGuiMouseCursor_ResizeEW:
        return cursor_shape::resize_ew;
    case ImGuiMouseCursor_ResizeNESW:
        return cursor_shape::resize_nesw;
    case ImGuiMouseCursor_ResizeNWSE:
        return cursor_shape::resize_nwse;
    case ImGuiMouseCursor_Hand:
        return cursor_shape::pointer;
    case ImGuiMouseCursor_Wait:
        return cursor_shape::wait;
    case ImGuiMouseCursor_Progress:
        return cursor_shape::progress;
    case ImGuiMouseCursor_NotAllowed:
        return cursor_shape::not_allowed;
    default:
        // Covers ImGuiMouseCursor_Arrow and ImGuiMouseCursor_None.
        // None means "draw nothing", which is a visibility decision the caller makes before reaching here — see imgui_context::begin_frame.
        return cursor_shape::arrow;
    }
}
} // namespace sr::impl
