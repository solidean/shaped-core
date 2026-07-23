cmake_minimum_required(VERSION 3.28)
project(imgui LANGUAGES CXX)

# Vendored Dear ImGui bundle, docking branch (see vendor-imgui.py for the pinned versions).
# ImGui core plus two addons drawn through it — ImPlot (plotting) and ImGuizmo (manipulation gizmos) — all compiled into this one target.
# Upstream's per-API backends are deliberately not vendored: we draw ImGui through sg, and that renderer lives in libs/graphics/shaped-rendering.
# The demos (imgui_demo, implot_demo) are the payload the renderer's GPU test draws.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(imgui STATIC
    ${IMGUI_DIR}/src/imgui.cpp
    ${IMGUI_DIR}/src/imgui_draw.cpp
    ${IMGUI_DIR}/src/imgui_tables.cpp
    ${IMGUI_DIR}/src/imgui_widgets.cpp
    ${IMGUI_DIR}/src/imgui_demo.cpp

    # ImPlot — immediate-mode plotting, built against the imgui headers above.
    ${IMGUI_DIR}/src/implot.cpp
    ${IMGUI_DIR}/src/implot_items.cpp
    ${IMGUI_DIR}/src/implot_demo.cpp

    # ImGuizmo — 3D manipulation gizmos.
    ${IMGUI_DIR}/src/imguizmo.cpp
)

# The headers live under include/imgui/, so a consumer includes <imgui/imgui.h>, <imgui/implot.h>, ... — the whole bundle namespaced under one dir.
# SYSTEM so the vendored headers do not trip our warnings in the consuming TU.
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR}/include)

# The vendored .cpp/.h include each other by bare name ("imgui.h", "implot.h", ...).
# PRIVATE include/imgui makes that resolve for our build only; a consumer never gets it and must use the <imgui/...> form.
target_include_directories(imgui PRIVATE ${IMGUI_DIR}/include/imgui)

# shaped-code's own additions to the vendored library: forward declarations (<imgui/imgui_fwd.hh>), the injected user config (<imgui/imgui_config.hh>), and the interop umbrella (<imgui/imgui_sc.hh>).
# They sit under shaped/imgui/ so they include as <imgui/...> alongside the vendored headers.
# Deliberately outside include/ and src/: vendor-imgui.py wipes those on every re-vendor, so a customization placed there would be lost.
# NOT SYSTEM — this is our code, held to our warnings, unlike the vendored sources next to it.
target_include_directories(imgui PUBLIC ${IMGUI_DIR}/shaped)

# PUBLIC, not PRIVATE: these change what imgui.h *declares*, so a consumer compiling without them would see a different ImGuiIO layout than imgui.cpp was built with — a silent ODR violation rather than a link error.
# This is also why the vendored imconfig.h stays byte-identical to upstream: vendor-imgui.py wipes include/ on every re-vendor, so an edit there would be lost on the next bump.
# Allocation is routed through mimalloc at runtime instead, via ImGui::SetAllocatorFunctions (see sr::imgui_context).
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_OBSOLETE_KEYIO

    # Scope the bundled stb (rect-pack, truetype) into a namespace, so nothing of stb's reaches global scope from us.
    #
    # This is the ODR guard against another library in the same program pulling in its own stb.
    # The *functions* were already safe — imgui compiles them with STBRP_STATIC / STBTT_STATIC, i.e. internal linkage — but the *types* (stbtt_fontinfo, stbrp_context, ...) were still declared at global scope.
    # Two differing definitions of one global type in a program is an ODR violation, and one the linker will not report.
    #
    # imgui's own hook, not a patch of ours: rewriting symbols in the vendored sources would have to be re-derived on every version bump and would silently miss whatever upstream adds.
    # imstb_textedit.h is already unconditionally namespaced ImStb upstream, so this puts all three stb pieces in one place.
    #
    # PUBLIC for the same reason as the flags above, and more sharply: imgui_internal.h reads this macro to form the stbrp_node_im typedef, so a consumer compiling without it would see a different type.
    IMGUI_STB_NAMESPACE=ImStb

    # Our injected user config (shaped/imgui/imgui_config.hh), which imgui.h includes just before imconfig.h.
    # It adds the tg::vec2f / tg::vec4f cast members to ImVec2 / ImVec4.
    # PUBLIC and ODR-critical exactly like the flags above: it changes what ImVec2 *is*, so every TU must compile with it or the type differs across the program.
    # The header pulls in nothing — tg is only forward-declared there — so this does not put typed-geometry onto the imgui target.
    IMGUI_USER_CONFIG="imgui/imgui_config.hh"
)

target_compile_features(imgui PUBLIC cxx_std_11)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(imgui PRIVATE /W0)
else()
    target_compile_options(imgui PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET imgui PROPERTY POSITION_INDEPENDENT_CODE ON)
