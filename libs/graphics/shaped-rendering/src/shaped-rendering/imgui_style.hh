#pragma once

#include <imgui/imgui_fwd.hh>

namespace sr
{
/// The Solidean dark theme for Dear ImGui.
/// The brand violet (#6830FF) and its lit variant (#A37BFF) as accents — down to the separators, borders and resize grips —
/// with code cyan (#5FC0D0) for the highlights that must read against the violet, all on the near-black #0b0d12 ground.
///
/// Writes every ImGuiCol slot plus the geometry that goes with the palette (roundings, spacing, borderless frames) into `style`.
/// Fields it does not name keep their incoming value, so pass a default-constructed ImGuiStyle for a clean result.
///
/// The colors are sRGB-encoded, which is what sr::imgui_routine draws unconverted —
/// the target must be a non-srgb format, or the theme comes out washed out (see libs/graphics/shaped-rendering/docs/imgui.md).
void apply_solidean_default_style(ImGuiStyle& style);

/// Applies the theme to the current context's style, ImGui::GetStyle().
/// Requires a current ImGui context — e.g. right after sr::imgui_context::create().
void apply_solidean_default_style();
} // namespace sr
