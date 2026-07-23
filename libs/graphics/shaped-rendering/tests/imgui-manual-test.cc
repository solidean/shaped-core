#include <clean-core/string/print.hh>
#include <imgui/imgui.h>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

#include <chrono>

// The whole stack in one loop, driven by a person: a real window, a real swapchain, imgui rendered through sg.
//
//     uv run dev.py test "sr - imgui window (manual)" --manual --mirror-test-output --timeout 0
//
// `--timeout 0` is not optional: dev.py kills a test binary after 60s by default, and this one runs until you close the window.
//
// Everything the automated tests cannot reach is here.
// They run headless against an offscreen target, so what they cannot check is exactly what a person is good at:
// does the UI respond, does the cursor change shape over a resize handle, does typing compose, does ctrl+V paste what you copied elsewhere.
//
// Close the window to end it.

#if SR_HAS_WINDOW

namespace
{
/// What to try, shown in the window so the test explains itself rather than living in a comment nobody reads.
void draw_guide(sr::imgui_context const& imgui, sr::window_system const& wsys, float fps)
{
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_FirstUseEver);
    ImGui::Begin("shaped-rendering — what to try");

    ImGui::Text("%.1f FPS", double(fps));
    ImGui::Separator();

    ImGui::SeparatorText("Input");
    ImGui::BulletText("Hover, click and drag the demo window.");
    ImGui::BulletText("Scroll inside a list — it should track the wheel with no lag.");
    ImGui::BulletText("Middle-click something. It must NOT act like a right-click.");

    ImGui::SeparatorText("Cursor shapes");
    ImGui::BulletText("Hover a window's edge and corner: the pointer becomes a resize arrow.");
    ImGui::BulletText("Hover a text field: it becomes an I-beam.");
    ImGui::BulletText("Over this 3D-view-less background imgui lets go, so the arrow returns.");

    ImGui::SeparatorText("Text, IME and clipboard");
    static char text[128] = "type here";
    ImGui::InputText("field", text, IM_ARRAYSIZE(text));
    ImGui::BulletText("Typing works, and so do accents and an IME if you have one.");
    ImGui::BulletText("ctrl+C here, then paste into another app — and the other way round.");
    if (ImGui::Button("copy a marker to the clipboard"))
        ImGui::SetClipboardText("shaped-core imgui says hello");
    ImGui::SameLine();
    if (ImGui::Button("read the clipboard"))
        cc::println("clipboard: {}", wsys.clipboard_text());

    ImGui::SeparatorText("Docking");
    ImGui::BulletText("Drag a window by its title bar onto an edge of another to dock it.");
    ImGui::BulletText("Drag it out again to undock. Tabs form when two dock to one spot.");

    ImGui::SeparatorText("Multi-viewport");
    ImGui::BulletText("Drag a window by its title bar right off this one — it becomes its own OS window.");
    ImGui::BulletText("Move and resize that window; its title bar and edges are the OS's, not imgui's.");
    ImGui::BulletText("Drag it back in — it merges into this window again.");
    ImGui::BulletText("Click the detached window: focus follows, and typing goes to whatever it holds.");
    ImGui::TextWrapped("Each detached window gets its own swapchain, created the first frame it appears and "
                       "resized from the window as you drag its edge.");

    ImGui::Separator();
    ImGui::Text("imgui wants: mouse=%s keyboard=%s", imgui.wants_mouse() ? "yes" : "no",
                imgui.wants_keyboard() ? "yes" : "no");

    ImGui::End();
}
} // namespace

TEST("sr - imgui window (manual)", nx::config::manual)
{
    auto const wsys = sr::window_system::create();
    auto const win = wsys->create_window({.title = "shaped-rendering — close this window to end the test", //
                                          .width = 1440,
                                          .height = 900});

    // A real adapter by preference — this is meant to be looked at — but WARP renders it just as correctly, only slower, so a machine without a usable D3D12 GPU still gets to run the test.
    auto const ctx = [&]
    {
        auto hardware = sg::create_dx12_context({});
        if (hardware.has_value())
            return hardware.value();

        cc::println("no hardware D3D12 adapter ({}) — falling back to WARP", hardware.error().to_string());
        auto warp = sg::create_dx12_context({.use_warp = true});
        REQUIRE(warp.has_value());
        return warp.value();
    }();

    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());

    slib::shader_library lib;
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sr::shader_package());

    // bgra8_unorm, not its _srgb sibling: imgui's colors are already sRGB-encoded and the routine refuses a target that would encode them twice.
    auto const sc = ctx->create_swapchain(
        {.native_window_handle = win->native_window_handle(), .format = sg::pixel_format::bgra8_unorm});

    auto imgui = sr::imgui_context::create({.enable_viewports = true}); // create() applies the Solidean theme by default

    cc::println("opened {}x{}; close the window to end", win->width(), win->height());

    auto last_time = std::chrono::steady_clock::now();
    auto smoothed_fps = 0.0f;

    while (!win->is_close_requested())
    {
        wsys->poll_events();
        imgui.process_events(*wsys);

        // A minimized window is 0x0, and sizing a swapchain against that is what acquire_backbuffer's auto-resize would otherwise try to do.
        if (win->is_minimized())
            continue;

        auto const now = std::chrono::steady_clock::now();
        auto const delta_time = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        smoothed_fps = smoothed_fps == 0.0f ? 1.0f / delta_time : smoothed_fps * 0.95f + (1.0f / delta_time) * 0.05f;

        imgui.begin_frame(*win, delta_time);

        // A dockspace over the whole window, so the demo windows have somewhere to dock.
        // Without it docking is only window-to-window and the feature looks half-present.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        draw_guide(imgui, *wsys, smoothed_fps);
        ImGui::ShowDemoWindow();

        imgui.end_frame();

        // imgui owns this window, so the batteries-included path renders the whole frame — the main window and every secondary viewport — in one call.
        // See sr::imgui_routine::execute for the compositing path.
        // The #0b0d12 brand ground, so the central dockspace matches the theme's window backgrounds.
        sr::render_imgui(imgui, *ctx, *sc, tg::vec4f(0.043f, 0.051f, 0.071f, 1.0f));
        ctx->advance_epoch(sc->buffer_count());
    }

    // Drain before the swapchain and the window go away: the last frames are still in flight.
    ctx->advance_epoch_and_wait_for_idle();
    cc::println("close requested — shutting down");
}

#endif // SR_HAS_WINDOW
