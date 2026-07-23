#pragma once

#include <imgui/impl/imgui_cc.hh>
#include <imgui/impl/imgui_tg.hh>

/// shaped-code interop for the vendored imgui bundle — the single public header for our additions.
///
/// A shaped-code addition to the vendored library, living in `extern/imgui/shaped/imgui/` so a re-vendor leaves it alone.
///
/// Include this in a translation unit that wants imgui to speak clean-core and typed-geometry:
///   - cc::string / cc::string_view display and editing (TextUnformatted, InputText, ...)
///   - implicit ImVec2 / ImVec4 <-> tg::vec2f / tg::vec4f conversions
///
/// The pieces sit under impl/; pull them in singly if you want only one side.
