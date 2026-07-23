#include <imgui/imgui.h>
#include <nexus/test.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/impl/imgui_input_translation.hh>
#include <shaped-rendering/window.hh>

// The sr-to-imgui input mapping, and the event feeding built on it.
//
// Reaches past sr's public API into impl/ for the same reason input-translation-test does one layer below:
// the mapping is a hundred table entries with one deliberate reordering, and there is no way to reach it through the public API.
//
// Needs no display and no device: the translation is pure, and an ImGui context is a plain heap object.

using namespace sr;

TEST("sr::impl - scancodes map to imgui keys")
{
    CHECK(impl::imgui_key_from_scancode(scancode::a) == ImGuiKey_A);
    CHECK(impl::imgui_key_from_scancode(scancode::z) == ImGuiKey_Z);
    CHECK(impl::imgui_key_from_scancode(scancode::num_0) == ImGuiKey_0);
    CHECK(impl::imgui_key_from_scancode(scancode::num_9) == ImGuiKey_9);
    CHECK(impl::imgui_key_from_scancode(scancode::f1) == ImGuiKey_F1);
    CHECK(impl::imgui_key_from_scancode(scancode::f12) == ImGuiKey_F12);
    CHECK(impl::imgui_key_from_scancode(scancode::space) == ImGuiKey_Space);
    CHECK(impl::imgui_key_from_scancode(scancode::escape) == ImGuiKey_Escape);

    // The editing keys imgui's text fields actually route on — a wrong entry here breaks typing, not movement.
    CHECK(impl::imgui_key_from_scancode(scancode::backspace) == ImGuiKey_Backspace);
    CHECK(impl::imgui_key_from_scancode(scancode::del) == ImGuiKey_Delete);
    CHECK(impl::imgui_key_from_scancode(scancode::home) == ImGuiKey_Home);
    CHECK(impl::imgui_key_from_scancode(scancode::end) == ImGuiKey_End);
    CHECK(impl::imgui_key_from_scancode(scancode::left) == ImGuiKey_LeftArrow);
    CHECK(impl::imgui_key_from_scancode(scancode::right) == ImGuiKey_RightArrow);

    // enter and the keypad's are distinct keys, exactly as they are distinct positions in sr.
    CHECK(impl::imgui_key_from_scancode(scancode::enter) == ImGuiKey_Enter);
    CHECK(impl::imgui_key_from_scancode(scancode::kp_enter) == ImGuiKey_KeypadEnter);
    CHECK(impl::imgui_key_from_scancode(scancode::enter) != impl::imgui_key_from_scancode(scancode::kp_enter));

    // Left and right modifiers stay distinct.
    CHECK(impl::imgui_key_from_scancode(scancode::left_ctrl) == ImGuiKey_LeftCtrl);
    CHECK(impl::imgui_key_from_scancode(scancode::right_ctrl) == ImGuiKey_RightCtrl);

    // imgui spells these differently from sr, which is where a mapping slips.
    CHECK(impl::imgui_key_from_scancode(scancode::equals) == ImGuiKey_Equal);
    CHECK(impl::imgui_key_from_scancode(scancode::grave) == ImGuiKey_GraveAccent);
    CHECK(impl::imgui_key_from_scancode(scancode::kp_minus) == ImGuiKey_KeypadSubtract);
    CHECK(impl::imgui_key_from_scancode(scancode::kp_plus) == ImGuiKey_KeypadAdd);
    CHECK(impl::imgui_key_from_scancode(scancode::kp_period) == ImGuiKey_KeypadDecimal);

    // An unmodelled position reads as None rather than as a neighbour.
    CHECK(impl::imgui_key_from_scancode(scancode::unknown) == ImGuiKey_None);
}

TEST("sr::impl - imgui cursors map to sr shapes")
{
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_Arrow) == cursor_shape::arrow);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_TextInput) == cursor_shape::text);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_Hand) == cursor_shape::pointer);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_NotAllowed) == cursor_shape::not_allowed);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_Wait) == cursor_shape::wait);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_Progress) == cursor_shape::progress);

    // ResizeAll is a move, not a resize — imgui uses it for dragging a whole window.
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_ResizeAll) == cursor_shape::move);

    // The two diagonals are the pair most easily swapped, and swapping them looks almost right.
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_ResizeNESW) == cursor_shape::resize_nesw);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_ResizeNWSE) == cursor_shape::resize_nwse);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_ResizeNS) == cursor_shape::resize_ns);
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_ResizeEW) == cursor_shape::resize_ew);

    // None means "draw nothing", a visibility decision the caller makes; it has no shape of its own.
    CHECK(impl::cursor_shape_from_imgui(ImGuiMouseCursor_None) == cursor_shape::arrow);
}

TEST("sr::impl - mouse buttons are reordered for imgui, not passed through")
{
    // This is the whole hazard: sr is left/middle/right, imgui is left/right/middle.
    // Passing the enum through unconverted swaps middle and right, which looks fine until someone middle-clicks.
    CHECK(impl::imgui_mouse_button_from(mouse_button::left) == 0);
    CHECK(impl::imgui_mouse_button_from(mouse_button::right) == 1);
    CHECK(impl::imgui_mouse_button_from(mouse_button::middle) == 2);
    CHECK(impl::imgui_mouse_button_from(mouse_button::x1) == 3);
    CHECK(impl::imgui_mouse_button_from(mouse_button::x2) == 4);

    // Spelled out, because the two enums agreeing on left is what hides the disagreement on the other two.
    CHECK(impl::imgui_mouse_button_from(mouse_button::middle) != int(mouse_button::middle));
    CHECK(impl::imgui_mouse_button_from(mouse_button::right) != int(mouse_button::right));
}

TEST("sr::imgui_context - a mouse click reaches imgui")
{
    auto imgui = sr::imgui_context::create();

    // A press, then a frame for imgui to see it in.
    imgui.process_event(
        {.payload
         = mouse_button_event{.button = mouse_button::left, .is_down = true, .cursor_pos = tg::pos2f(40.0f, 30.0f)}});
    imgui.begin_frame({.display_size = tg::vec2i(200, 100)});

    CHECK(ImGui::GetIO().MousePos.x == 40.0f);
    CHECK(ImGui::GetIO().MousePos.y == 30.0f);
    CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    CHECK(!ImGui::IsMouseDown(ImGuiMouseButton_Right));

    imgui.end_frame();
}

TEST("sr::imgui_context - a middle click does not read as a right click")
{
    // The end-to-end form of the reorder check above: feeding sr's enum straight through would land here.
    auto imgui = sr::imgui_context::create();

    imgui.process_event(
        {.payload
         = mouse_button_event{.button = mouse_button::middle, .is_down = true, .cursor_pos = tg::pos2f(1.0f, 1.0f)}});
    imgui.begin_frame({.display_size = tg::vec2i(200, 100)});

    CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Middle));
    CHECK(!ImGui::IsMouseDown(ImGuiMouseButton_Right));

    imgui.end_frame();
}

TEST("sr::imgui_context - a key press and its modifiers reach imgui")
{
    auto imgui = sr::imgui_context::create();

    imgui.process_event(
        {.payload = key_event{.scancode = scancode::z, .modifiers = key_modifiers::ctrl, .is_down = true}});
    imgui.begin_frame({.display_size = tg::vec2i(200, 100)});

    CHECK(ImGui::IsKeyDown(ImGuiKey_Z));
    CHECK(ImGui::GetIO().KeyCtrl);
    CHECK(!ImGui::GetIO().KeyShift);

    imgui.end_frame();
}

TEST("sr::imgui_context - text arrives as decoded UTF-8 codepoints")
{
    // Text must come from text_event, not rebuilt from key_events: an IME commits a whole phrase and a paste arrives as one event.
    //
    // The multi-byte character is the point.
    // It proves two things a plain "abc" would not:
    // that imgui decoded UTF-8 rather than taking one character per byte,
    // and that a cc::string — which is not null-terminated — survived the handover to imgui's C-string API with the right length.
    auto imgui = sr::imgui_context::create();

    imgui.process_event({.payload = text_event{.text = "hé"}});

    // Feeding only queues; NewFrame is what commits into io.InputQueueCharacters.
    imgui.begin_frame({.display_size = tg::vec2i(200, 100)});

    REQUIRE(ImGui::GetIO().InputQueueCharacters.Size == 2); // two codepoints, not the three UTF-8 bytes
    CHECK(ImGui::GetIO().InputQueueCharacters[0] == 'h');
    CHECK(ImGui::GetIO().InputQueueCharacters[1] == 0x00E9); // é

    imgui.end_frame();
}

TEST("sr::imgui_context - the scroll wheel reaches imgui on the same frame")
{
    // The frame it lands on is the point, not merely that it arrives.
    // imgui's ConfigInputTrickleEventQueue splits a cursor-position change and a wheel across two frames,
    // so restating the wheel's own cursor_pos — which imgui already knows from the motion events — would delay every scroll by a frame.
    auto imgui = sr::imgui_context::create();

    imgui.process_event(
        {.payload = mouse_wheel_event{.delta = tg::vec2f(0.0f, 2.5f), .cursor_pos = tg::pos2f(5.0f, 6.0f)}});

    imgui.begin_frame({.display_size = tg::vec2i(200, 100)});
    auto const wheel_frame_1 = ImGui::GetIO().MouseWheel;
    imgui.end_frame();

    imgui.begin_frame({.display_size = tg::vec2i(200, 100)});
    auto const wheel_frame_2 = ImGui::GetIO().MouseWheel;
    imgui.end_frame();

    CHECK(wheel_frame_1 == 2.5f); // this frame, not the next
    CHECK(wheel_frame_2 == 0.0f); // and not applied twice
}

#if SR_HAS_WINDOW

TEST("sr::imgui_context - a frame driven by a window takes its size and text-input intent")
{
    // The window-driven path end to end, on the dummy video driver.
    // What it pins is the wiring, not the pixels:
    // that begin_frame reads the window rather than taking a hand-passed size,
    // and that imgui's text-input intent reaches the window — which is what makes an IME and an on-screen keyboard engage.
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "imgui", .width = 640, .height = 480});

    auto imgui = sr::imgui_context::create();

    // Nothing has focus, so imgui wants no text input and the window must not be left listening.
    imgui.begin_frame(*win, 1.0f / 60.0f);
    CHECK(ImGui::GetIO().DisplaySize.x == 640.0f);
    CHECK(ImGui::GetIO().DisplaySize.y == 480.0f);
    CHECK(!win->is_text_input_active());
    imgui.end_frame();

    // An empty event stream is the common case and must be a no-op, not an assert.
    wsys->poll_events();
    imgui.process_events(*wsys);

    imgui.begin_frame(*win, 1.0f / 60.0f);
    CHECK(!imgui.wants_keyboard()); // no widget has focus
    imgui.end_frame();
}

TEST("sr::imgui_context - imgui drives the window system's cursor, and only when it wants the mouse")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.width = 320, .height = 240});

    auto imgui = sr::imgui_context::create();

    auto const draw_ui = []
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(200, 200));
        ImGui::Begin("hover me");
        ImGui::End();
    };

    // Pointer well outside the imgui window: imgui does not want the mouse, so the caller's cursor stands.
    wsys->set_cursor(cursor_shape::crosshair);
    imgui.process_event({.window = win.get(), .payload = mouse_move_event{.cursor_pos = tg::pos2f(300.0f, 220.0f)}});
    for (auto i = 0; i < 3; ++i)
    {
        imgui.begin_frame(*win, 1.0f / 60.0f);
        draw_ui();
        imgui.end_frame();
    }
    CHECK(!imgui.wants_mouse());
    CHECK(wsys->cursor() == cursor_shape::crosshair); // untouched

    // Move it over the imgui window.
    // Three frames, because the effect is two behind the input:
    // imgui decides hover at NewFrame from the previous frame's layout, and begin_frame applies the cursor from the previous frame's decision.
    imgui.process_event({.window = win.get(), .payload = mouse_move_event{.cursor_pos = tg::pos2f(30.0f, 30.0f)}});
    for (auto i = 0; i < 3; ++i)
    {
        imgui.begin_frame(*win, 1.0f / 60.0f);
        draw_ui();
        imgui.end_frame();
    }

    // Viewports are off, so imgui coordinates stay relative to the window and the position goes in unchanged.
    CHECK(ImGui::GetIO().MousePos.x == 30.0f);
    CHECK(ImGui::GetIO().MousePos.y == 30.0f);

    REQUIRE(imgui.wants_mouse());                 // otherwise the cursor check below proves nothing
    CHECK(wsys->cursor() == cursor_shape::arrow); // imgui's now, no longer the crosshair
}

TEST("sr::imgui_context - viewports are off unless asked for, and then move imgui into desktop space")
{
    // The whole point of the opt-in: enabling viewports changes what an imgui coordinate means, so a caller that did not ask for them must see exactly the single-viewport behaviour pinned above.
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.width = 320, .height = 240});

    // A window manager places a window where it likes, and this test is only meaningful off the origin.
    win->set_position(tg::pos2i(140, 90));

    auto imgui = sr::imgui_context::create({.enable_viewports = true});

    imgui.begin_frame(*win, 1.0f / 60.0f);
    imgui.end_frame();
    imgui.update_viewports();

    // The main viewport now describes the OS window rather than a rectangle at the origin.
    CHECK(ImGui::GetMainViewport()->Pos.x == 140.0f);
    CHECK(ImGui::GetMainViewport()->Pos.y == 90.0f);

    imgui.process_event({.window = win.get(), .payload = mouse_move_event{.cursor_pos = tg::pos2f(10.0f, 20.0f)}});
    imgui.begin_frame(*win, 1.0f / 60.0f);
    imgui.end_frame();
    imgui.update_viewports();

    // A window-relative (10, 20) is desktop (150, 110) — imgui hit-tests viewports with this, so an untranslated position would name the wrong one as soon as a second viewport exists.
    CHECK(ImGui::GetIO().MousePos.x == 150.0f);
    CHECK(ImGui::GetIO().MousePos.y == 110.0f);
}

TEST("sr::imgui_context - a frame that skips update_viewports asserts")
{
    // Without it imgui stops hit-testing the mouse against any viewport, which reads as "nothing hovers any more" rather than as a missing call.
    // Worth an assert instead of a debugging session.
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.width = 320, .height = 240});

    auto imgui = sr::imgui_context::create({.enable_viewports = true});

    imgui.begin_frame(*win, 1.0f / 60.0f);
    imgui.end_frame();
    // update_viewports() deliberately missing here

    CHECK_ASSERTS(imgui.begin_frame(*win, 1.0f / 60.0f));
}

TEST("sr::imgui_context - copy and paste reach the system clipboard")
{
    // Wires imgui's clipboard hooks to sr::window_system, so ctrl+C and ctrl+V in a text field talk to the real clipboard rather than to nothing.
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.width = 320, .height = 240});

    auto imgui = sr::imgui_context::create();

    // The hooks are installed on the first window-driven frame, not at create() — that is the first point a window_system is in reach.
    imgui.begin_frame(*win, 1.0f / 60.0f);
    imgui.end_frame();

    wsys->set_clipboard_text("from the os");
    CHECK(cc::string_view(ImGui::GetClipboardText()) == "from the os");

    ImGui::SetClipboardText("from imgui");
    CHECK(wsys->clipboard_text() == "from imgui");
}

#endif // SR_HAS_WINDOW
