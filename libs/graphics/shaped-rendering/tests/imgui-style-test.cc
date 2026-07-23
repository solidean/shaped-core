#include <imgui/imgui.h>
#include <nexus/test.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/imgui_style.hh>

namespace
{
// The palette values, as the float the theme resolves each hex to — recomputed here so a wrong slot is caught, not just a wrong shape.
constexpr float chan(unsigned byte)
{
    return float(byte) / 255.0f;
}
} // namespace

TEST("sr - the solidean-default style paints the brand palette")
{
    // ImGuiStyle's default ctor fills every slot (StyleColorsDark) and needs no context, so this runs device-free.
    ImGuiStyle style;
    sr::apply_solidean_default_style(style);

    // Text is #e8ecf1, opaque.
    CHECK(style.Colors[ImGuiCol_Text].x == chan(0xe8));
    CHECK(style.Colors[ImGuiCol_Text].y == chan(0xec));
    CHECK(style.Colors[ImGuiCol_Text].z == chan(0xf1));
    CHECK(style.Colors[ImGuiCol_Text].w == 1.0f);

    // The window ground is #0b0d12.
    CHECK(style.Colors[ImGuiCol_WindowBg].x == chan(0x0b));
    CHECK(style.Colors[ImGuiCol_WindowBg].y == chan(0x0d));
    CHECK(style.Colors[ImGuiCol_WindowBg].z == chan(0x12));

    // Links take the site's #A37BFF, opaque.
    CHECK(style.Colors[ImGuiCol_TextLink].x == chan(0xa3));
    CHECK(style.Colors[ImGuiCol_TextLink].y == chan(0x7b));
    CHECK(style.Colors[ImGuiCol_TextLink].z == chan(0xff));
    CHECK(style.Colors[ImGuiCol_TextLink].w == 1.0f);

    // A translucent overlay keeps the brand RGB and only drops alpha.
    CHECK(style.Colors[ImGuiCol_Header].x == chan(0x68));
    CHECK(style.Colors[ImGuiCol_Header].w == 0.22f);

    // The structural lines are accented, not neutral: separators carry the brand violet.
    CHECK(style.Colors[ImGuiCol_Separator].x == chan(0x68));
    CHECK(style.Colors[ImGuiCol_Separator].y == chan(0x30));
    CHECK(style.Colors[ImGuiCol_Separator].z == chan(0xff));
    CHECK(style.Colors[ImGuiCol_Separator].w == 0.30f);
}

TEST("sr - a context applies the solidean theme by default")
{
    auto imgui = sr::imgui_context::create();

    // No explicit apply: the #6830FF violet reaching the slider grab means create() styled the context itself.
    auto const& grab = ImGui::GetStyle().Colors[ImGuiCol_SliderGrab];
    CHECK(grab.x == chan(0x68));
    CHECK(grab.y == chan(0x30));
    CHECK(grab.z == chan(0xff));
}

TEST("sr - a context leaves the theme alone when asked")
{
    auto imgui = sr::imgui_context::create({.apply_default_style = false});

    // The stock imgui dark leaves the slider grab a desaturated blue-gray, so a red channel at the brand's is proof the opt-out held.
    CHECK(ImGui::GetStyle().Colors[ImGuiCol_SliderGrab].x != chan(0x68));
}
