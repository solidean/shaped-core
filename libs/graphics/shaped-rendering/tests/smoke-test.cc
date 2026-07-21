#include <clean-core/string/string_view.hh>
#include <imgui.h>
#include <nexus/test.hh>
#include <shaped-rendering/all.hh>

// Confirms the library builds, links, and its umbrella header compiles.
TEST("sr smoke - links")
{
    CHECK(true);
}

// Proves the vendored imgui actually links (not just compiles) and pins the version we vendored, so a
// re-vendor that lands a different commit fails here rather than somewhere subtler. Bump alongside
// PIN_TAG in extern/imgui/vendor-imgui.py.
TEST("sr smoke - imgui links at the pinned version")
{
    CHECK(cc::string_view(ImGui::GetVersion()) == "1.92.8");
}
