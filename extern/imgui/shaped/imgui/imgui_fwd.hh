#pragma once

/// Forward declarations for Dear ImGui's public types.
///
/// A shaped-code addition to the vendored library, not an upstream file.
/// It lives in `extern/imgui/shaped/imgui/`, outside `include/` and `src/`, so `vendor-imgui.py` — which wipes those on every re-vendor — leaves it alone.
///
/// Include this from a header that only *names* an imgui type.
/// sr's public headers do, to keep `<imgui/imgui.h>` and its transitive weight out of a consumer's translation unit;
/// include `<imgui/imgui.h>` itself where the definitions are actually used (a `.cc`, or an `impl/` header).
///
/// Only opaque struct types belong here.
/// imgui's typedefs (`ImTextureID`, `ImU32`, …) and enums (`ImGuiKey`, …) cannot be forward-declared, so a header that needs those includes `<imgui/imgui.h>` instead.
///
/// Extend as needed — one of the customization points that grow alongside the vendored library (cc interop, ImGuizmo, ImPlot, …).

// context, IO and platform windows
struct ImGuiContext;
struct ImGuiIO;
struct ImGuiViewport;
struct ImGuiPlatformIO;

// draw data a renderer backend consumes
struct ImDrawData;
struct ImDrawList;
struct ImDrawVert;

// textures and fonts
struct ImTextureData;
struct ImFontAtlas;
