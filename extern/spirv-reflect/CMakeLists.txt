cmake_minimum_required(VERSION 3.28)
project(spirv-reflect LANGUAGES C)

# Vendored SPIRV-Reflect (see vendor-spirv-reflect.py for the pinned tag).
# One static target from upstream's single implementation TU; the SPIR-V grammar header it needs is vendored beside it
# under include/spirv/unified1/, which is where spirv_reflect.h looks for it.
set(SPIRV_REFLECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(spirv-reflect STATIC ${SPIRV_REFLECT_DIR}/src/spirv_reflect.c)

# SYSTEM so the upstream headers do not trip our warnings in the consuming TU.
target_include_directories(spirv-reflect SYSTEM PUBLIC ${SPIRV_REFLECT_DIR}/include)

# spirv_reflect.h otherwise reaches its grammar header by the relative path it sits at in upstream's tree
# ("./include/spirv/unified1/spirv.h"), which only resolves from upstream's own root.
# PUBLIC because the consuming TU includes that header too, and would take the same broken path without it.
target_compile_definitions(spirv-reflect PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(spirv-reflect PRIVATE /W0)
else()
    target_compile_options(spirv-reflect PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET spirv-reflect PROPERTY POSITION_INDEPENDENT_CODE ON)
