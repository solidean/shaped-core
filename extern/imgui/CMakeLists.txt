cmake_minimum_required(VERSION 3.28)
project(imgui LANGUAGES CXX)

# Vendored Dear ImGui, docking branch (see vendor-imgui.py for the pinned version). Upstream's backends/
# are deliberately not vendored: we draw ImGui through sg, and that renderer lives in
# libs/graphics/shaped-rendering. This target is the core library plus the demo window, which the
# renderer's GPU test draws as its payload.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(imgui STATIC
    ${IMGUI_DIR}/src/imgui.cpp
    ${IMGUI_DIR}/src/imgui_draw.cpp
    ${IMGUI_DIR}/src/imgui_tables.cpp
    ${IMGUI_DIR}/src/imgui_widgets.cpp
    ${IMGUI_DIR}/src/imgui_demo.cpp
)

# SYSTEM so imgui.h does not trip our warnings in the consuming TU.
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR}/include)

# PUBLIC, not PRIVATE: these change what imgui.h *declares*, so a consumer compiling without them would
# see a different ImGuiIO layout than imgui.cpp was built with — a silent ODR violation rather than a link
# error. This is also why the vendored imconfig.h stays byte-identical to upstream: vendor-imgui.py wipes
# include/ on every re-vendor, so an edit there would be lost on the next bump. Allocation is routed
# through mimalloc at runtime instead, via ImGui::SetAllocatorFunctions (see sr::imgui_context).
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_OBSOLETE_KEYIO
)

target_compile_features(imgui PUBLIC cxx_std_11)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(imgui PRIVATE /W0)
else()
    target_compile_options(imgui PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET imgui PROPERTY POSITION_INDEPENDENT_CODE ON)
