#pragma once

/// Dear ImGui's injected user configuration.
///
/// A shaped-code addition to the vendored library, not an upstream file.
/// It lives in `extern/imgui/shaped/imgui/`, outside `include/` and `src/`, so `vendor-imgui.py` — which wipes those on every re-vendor — leaves it alone.
///
/// imgui.h reaches this through IMGUI_USER_CONFIG (set on the imgui CMake target), included just before imconfig.h.
/// The vendored imconfig.h stays byte-identical to upstream; whatever we customize lands here instead.
///
/// Today it teaches ImVec2 / ImVec4 to convert implicitly to and from tg::vec2f / tg::vec4f.
/// The conversion *members* are declared here, so they are baked into ImVec2 in every translation unit and the type has one definition everywhere — no ODR skew.
/// Their *definitions* live in <imgui/imgui_sc.hh> (via impl/imgui_tg.hh), pulled in only by a translation unit that actually wants the conversions;
/// that is the one place typed-geometry meets imgui.
///
/// This header includes nothing, on purpose.
/// It is part of imgui's own definition, compiled into imgui.cpp, so it must not drag typed-geometry onto the imgui target — which sits below the libs and must not depend upward on tg.
/// tg::vec is forward-declared instead; `tg::vec<2, float>` is exactly `tg::vec2f` (tg's f32 is float), so the cast targets name the real types without an include.

namespace tg
{
template <int D, class T>
struct vec;
} // namespace tg

/// Implicit ImVec2 <-> tg::vec2f conversions — definitions in <imgui/imgui_sc.hh>.
#define IM_VEC2_CLASS_EXTRA               \
    ImVec2(::tg::vec<2, float> const& v); \
    operator ::tg::vec<2, float>() const;

/// Implicit ImVec4 <-> tg::vec4f conversions — definitions in <imgui/imgui_sc.hh>.
#define IM_VEC4_CLASS_EXTRA               \
    ImVec4(::tg::vec<4, float> const& v); \
    operator ::tg::vec<4, float>() const;
