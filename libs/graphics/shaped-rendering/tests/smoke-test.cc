#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <imgui/imgui.h>
#include <imgui/imgui_sc.hh>
#include <nexus/test.hh>
#include <shaped-rendering/all.hh>
#include <typed-geometry/linalg/vec.hh>

// Confirms the library builds, links, and its umbrella header compiles.
TEST("sr smoke - links")
{
    CHECK(true);
}

// Proves the vendored imgui actually links (not just compiles) and pins the version we vendored, so a re-vendor that lands a different commit fails here rather than somewhere subtler.
// Bump alongside the ImGui commit in extern/imgui/vendor-imgui.py.
TEST("sr smoke - imgui links at the pinned version")
{
    CHECK(cc::string_view(ImGui::GetVersion()) == "1.92.8");
}

// Exercises the tg interop baked into ImVec2 / ImVec4 by <imgui_config.hh>, defined in <imgui_tg.hh>.
TEST("sr smoke - imgui tg interop round-trips")
{
    ImVec2 const iv2 = tg::vec2f(1.0f, 2.0f); // implicit tg::vec2f -> ImVec2
    CHECK(iv2.x == 1.0f);
    CHECK(iv2.y == 2.0f);
    tg::vec2f const back2 = iv2; // implicit ImVec2 -> tg::vec2f
    CHECK(back2 == tg::vec2f(1.0f, 2.0f));

    ImVec4 const iv4 = tg::vec4f(1.0f, 2.0f, 3.0f, 4.0f);
    tg::vec4f const back4 = iv4;
    CHECK(back4 == tg::vec4f(1.0f, 2.0f, 3.0f, 4.0f));
}

// Compile + link check for the cc interop in <imgui_cc.hh>: display plus the three editing overloads.
// They need a live imgui frame to run, so this is instantiated but never called.
[[maybe_unused]] static void imgui_cc_compile_check(cc::string& s, cc::string_view v)
{
    ImGui::TextUnformatted(v);
    ImGui::InputText("label", s);
    ImGui::InputTextMultiline("label", s);
    ImGui::InputTextWithHint("label", "hint", s);
}
