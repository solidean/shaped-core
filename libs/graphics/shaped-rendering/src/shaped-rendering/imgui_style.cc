#include <clean-core/common/asserts.hh>
#include <imgui/imgui.h>
#include <shaped-rendering/imgui_style.hh>

namespace sr
{
namespace
{
// 0xRRGGBB sRGB, opaque.
// The routine draws these unconverted, so the hex is the byte value that reaches the target — no gamma step.
[[nodiscard]] ImVec4 rgb(unsigned hex)
{
    auto const r = float((hex >> 16) & 0xff) / 255.0f;
    auto const g = float((hex >> 8) & 0xff) / 255.0f;
    auto const b = float(hex & 0xff) / 255.0f;
    return {r, g, b, 1.0f};
}

// The same color at a different opacity — for the translucent overlays (headers, drop targets, dim backgrounds).
[[nodiscard]] ImVec4 fade(ImVec4 c, float a)
{
    return {c.x, c.y, c.z, a};
}
} // namespace

void apply_solidean_default_style(ImGuiStyle& style)
{
    // The Solidean brand palette, named once so each assignment below reads as intent rather than a hex literal.
    auto const bg = rgb(0x0b0d12);       // page ground
    auto const fg = rgb(0xe8ecf1);       // primary text
    auto const muted = rgb(0xa9b1bd);    // secondary / disabled text
    auto const brand = rgb(0x6830ff);    // primary accent — the violet
    auto const brand_lt = rgb(0xa37bff); // links, marks, and the lit end of an accent
    auto const code = rgb(0x5fc0d0);     // code cyan — kept for the few highlights that must read against the violet
    auto const card = rgb(0x121622);     // raised surfaces: frames, popups, unfocused tabs
    auto const line = rgb(0x1d2331);     // borders and separators

    auto* const c = style.Colors;

    c[ImGuiCol_Text] = fg;
    c[ImGuiCol_TextDisabled] = muted;

    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = fade(bg, 0.0f); // let a child sit on whatever it is nested in
    c[ImGuiCol_PopupBg] = fade(card, 0.98f);
    c[ImGuiCol_Border] = fade(brand, 0.25f); // window / child / popup edges carry a faint violet
    c[ImGuiCol_BorderShadow] = fade(bg, 0.0f);

    c[ImGuiCol_FrameBg] = card;
    c[ImGuiCol_FrameBgHovered] = fade(brand, 0.18f); // a faint violet lift rather than a hard edge
    c[ImGuiCol_FrameBgActive] = fade(brand, 0.30f);

    c[ImGuiCol_TitleBg] = card;
    c[ImGuiCol_TitleBgActive] = fade(brand, 0.28f); // the focused window wears the brand
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = card;

    c[ImGuiCol_ScrollbarBg] = fade(bg, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = line;
    c[ImGuiCol_ScrollbarGrabHovered] = fade(muted, 0.50f);
    c[ImGuiCol_ScrollbarGrabActive] = brand;

    c[ImGuiCol_CheckMark] = brand_lt;
    c[ImGuiCol_CheckboxSelectedBg] = fade(brand, 0.35f);
    c[ImGuiCol_SliderGrab] = brand;
    c[ImGuiCol_SliderGrabActive] = brand_lt;

    c[ImGuiCol_Button] = fade(brand, 0.16f); // a violet base, not a flat gray, then solid on hover
    c[ImGuiCol_ButtonHovered] = brand;
    c[ImGuiCol_ButtonActive] = brand_lt;

    c[ImGuiCol_Header] = fade(brand, 0.22f); // CollapsingHeader, TreeNode, Selectable, MenuItem — a wash, not a slab
    c[ImGuiCol_HeaderHovered] = fade(brand, 0.38f);
    c[ImGuiCol_HeaderActive] = fade(brand, 0.55f);

    c[ImGuiCol_Separator] = fade(brand, 0.30f); // dividers read as violet lines, not neutral #1d2331
    c[ImGuiCol_SeparatorHovered] = brand;
    c[ImGuiCol_SeparatorActive] = brand_lt;

    c[ImGuiCol_ResizeGrip] = fade(brand, 0.22f);
    c[ImGuiCol_ResizeGripHovered] = fade(brand, 0.60f);
    c[ImGuiCol_ResizeGripActive] = brand;

    c[ImGuiCol_InputTextCursor] = fg;

    c[ImGuiCol_TabHovered] = fade(brand, 0.55f);
    c[ImGuiCol_Tab] = card;
    c[ImGuiCol_TabSelected] = fade(brand, 0.30f);
    c[ImGuiCol_TabSelectedOverline] = brand_lt;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = card;
    c[ImGuiCol_TabDimmedSelectedOverline] = fade(brand_lt, 0.0f); // no overline once the tab bar is unfocused

    c[ImGuiCol_DockingPreview] = fade(brand, 0.70f);
    c[ImGuiCol_DockingEmptyBg] = bg;

    c[ImGuiCol_PlotLines] = code;
    c[ImGuiCol_PlotLinesHovered] = brand_lt;
    c[ImGuiCol_PlotHistogram] = brand;
    c[ImGuiCol_PlotHistogramHovered] = brand_lt;

    c[ImGuiCol_TableHeaderBg] = card;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = fade(line, 0.50f);
    c[ImGuiCol_TableRowBg] = fade(bg, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = fade(card, 0.40f);

    c[ImGuiCol_TextLink] = brand_lt; // the site's link color
    c[ImGuiCol_TextSelectedBg] = fade(brand, 0.35f);
    c[ImGuiCol_TreeLines] = fade(brand, 0.30f); // hierarchy outlines share the separator's violet

    c[ImGuiCol_DragDropTarget] = code;
    c[ImGuiCol_DragDropTargetBg] = fade(code, 0.15f);
    c[ImGuiCol_UnsavedMarker] = brand_lt;

    c[ImGuiCol_NavCursor] = brand_lt;
    c[ImGuiCol_NavWindowingHighlight] = fade(fg, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = fade(bg, 0.40f);
    c[ImGuiCol_ModalWindowDimBg] = fade(bg, 0.60f);

    // Geometry: airy and softly rounded.
    // Frames carry no border — they read by their #121622 fill and a violet lift on hover, not by an outline —
    // so the surfaces stay calm and the #1d2331 lines are spent only where structure matters: window, child and popup edges.
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = 7.0f;
    style.ChildRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.ScrollbarRounding = 9.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
}

void apply_solidean_default_style()
{
    CC_ASSERT(ImGui::GetCurrentContext() != nullptr, "no current ImGui context — create one before applying a style");
    apply_solidean_default_style(ImGui::GetStyle());
}
} // namespace sr
