#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>
#include <imgui/imgui.h>
#include <nexus/test.hh>
#include <shaped-rendering/imgui_context.hh>

/// The settings seam: imgui's layout state as text a caller persists wherever it likes, instead of an imgui.ini in the working directory.
///
/// Every test here is device-free — begin_frame(frame_info) needs no window at all.
/// They share the process-global ImGui context, hence `exclusive`.

namespace
{
/// Runs `body` inside one frame of a context sized 800x600.
template <class F>
void in_frame(sr::imgui_context& imgui, F&& body)
{
    imgui.begin_frame({.display_size = tg::vec2i(800, 600)});
    body();
    imgui.end_frame();
}

/// Opens a window at `pos`, which is what puts an entry into the settings.
void draw_window_at(char const* name, ImVec2 pos)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(200, 120));
    ImGui::Begin(name);
    ImGui::End();
}
} // namespace

TEST("sr::imgui_context - no ini file is written by default", main_thread, exclusive("sr-imgui-context"))
{
    auto imgui = sr::imgui_context::create();

    // The regression pin for the whole seam: imgui's own default is "imgui.ini", relative to the working directory.
    CHECK(ImGui::GetIO().IniFilename == nullptr);

    in_frame(imgui, [] { draw_window_at("layout", ImVec2(120, 80)); });
}

TEST("sr::imgui_context - an ini_file hands persistence back to imgui", main_thread, exclusive("sr-imgui-context"))
{
    auto imgui = sr::imgui_context::create({.ini_file = "some-app.ini"});
    REQUIRE(ImGui::GetIO().IniFilename != nullptr);
    CHECK(cc::string_view(ImGui::GetIO().IniFilename) == "some-app.ini");

    // imgui keeps the pointer, so the string must not move when the context that owns it does.
    auto moved = cc::move(imgui);
    REQUIRE(ImGui::GetIO().IniFilename != nullptr);
    CHECK(cc::string_view(ImGui::GetIO().IniFilename) == "some-app.ini");
    CHECK(moved.is_valid());

    // No frame is run on purpose: the first one would read the file off disk, which is imgui's business rather than this test's.
}

TEST("sr::imgui_context - settings carry the windows a frame drew", main_thread, exclusive("sr-imgui-context"))
{
    auto imgui = sr::imgui_context::create();
    in_frame(imgui, [] { draw_window_at("layout", ImVec2(120, 80)); });

    auto const ini = imgui.settings();
    CHECK(ini.contains("[Window][layout]"));
    CHECK(ini.contains("Pos=120,80"));
    CHECK(ini.contains("Size=200,120"));
}

TEST("sr::imgui_context - load_settings places a window that sets no position", main_thread, exclusive("sr-imgui-context"))
{
    auto stored = cc::string();
    {
        auto imgui = sr::imgui_context::create();
        in_frame(imgui, [] { draw_window_at("layout", ImVec2(140, 90)); });
        stored = imgui.settings();
    }

    auto imgui = sr::imgui_context::create();
    imgui.load_settings(stored);

    auto position = ImVec2(0, 0);
    in_frame(imgui,
             [&]
             {
                 ImGui::Begin("layout"); // no SetNextWindowPos: the restored settings are the only thing that can place it
                 position = ImGui::GetWindowPos();
                 ImGui::End();
             });

    CHECK(position.x == 140.0f);
    CHECK(position.y == 90.0f);
}

TEST("sr::imgui_context - take_dirty_settings reports a change exactly once", main_thread, exclusive("sr-imgui-context"))
{
    auto imgui = sr::imgui_context::create();

    // imgui defers the flag by IniSavingRate seconds after a change, so a short rate is what lets the next frame raise it.
    ImGui::GetIO().IniSavingRate = 0.001f;

    // Nothing has happened yet.
    CHECK(!imgui.take_dirty_settings().has_value());

    // Creating a window is a settings change; the second frame is where the deferred flag comes up.
    in_frame(imgui, [] { draw_window_at("layout", ImVec2(60, 40)); });
    in_frame(imgui, [] { draw_window_at("layout", ImVec2(60, 40)); });

    auto const taken = imgui.take_dirty_settings();
    REQUIRE(taken.has_value());
    CHECK(taken.value().contains("[Window][layout]"));

    // Taking it cleared the flag, so an unchanged frame reports nothing.
    in_frame(imgui, [] { draw_window_at("layout", ImVec2(60, 40)); });
    CHECK(!imgui.take_dirty_settings().has_value());
}
