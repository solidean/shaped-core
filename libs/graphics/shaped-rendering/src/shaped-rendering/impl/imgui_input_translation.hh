#pragma once

#include <imgui/imgui.h>
#include <shaped-rendering/input.hh>

/// The sr-to-imgui input mapping, split out from imgui_context.cc so a test can drive it directly.
///
/// Internal: this header names ImGui types, so it lives under impl/ and is not part of the public header set.
///
/// The same reasoning as impl/input_translation.hh (which maps SDL to sr, one layer below):
/// these are pure functions of their arguments, they are a hundred-odd table entries with one deliberate reordering, and there is no way to reach them through the public API.
/// So they are tested at the seam rather than not at all.

namespace sr::impl
{
/// The ImGuiKey a physical position names, or ImGuiKey_None for one imgui does not model.
/// Deliberately partial, like its SDL counterpart: a position with no ImGuiKey must read as None rather than as some neighbouring key.
[[nodiscard]] ImGuiKey imgui_key_from_scancode(sr::scancode code);

/// imgui's mouse-button index.
///
/// The orders differ and that is the whole hazard: sr is left / middle / right, imgui is left / right / middle.
/// Passing an sr::mouse_button through unconverted swaps the middle and right buttons, which looks like nothing until someone middle-clicks.
[[nodiscard]] int imgui_mouse_button_from(sr::mouse_button button);

/// The pointer shape an ImGuiMouseCursor asks for.
///
/// ImGuiMouseCursor_None has no shape — it means draw nothing — so a caller must handle it before calling this;
/// it maps to arrow here rather than to some sentinel.
[[nodiscard]] sr::cursor_shape cursor_shape_from_imgui(int imgui_cursor);
} // namespace sr::impl
