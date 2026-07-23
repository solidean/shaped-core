#pragma once

#include <imgui/imgui.h>

#include <typed-geometry/linalg/vec.hh>

/// typed-geometry interop for Dear ImGui — the definitions behind the casts declared in <imgui/imgui_config.hh>.
/// Pulled in through the <imgui/imgui_sc.hh> umbrella; this is the one place the two libraries meet.

// tg::vec exposes its components through operator[], not named .x/.y members (yet).
inline ImVec2::ImVec2(tg::vec2f const& v) : x(v[0]), y(v[1]) {}
inline ImVec2::operator tg::vec2f() const { return tg::vec2f(x, y); }

inline ImVec4::ImVec4(tg::vec4f const& v) : x(v[0]), y(v[1]), z(v[2]), w(v[3]) {}
inline ImVec4::operator tg::vec4f() const { return tg::vec4f(x, y, z, w); }
