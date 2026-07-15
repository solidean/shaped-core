cmake_minimum_required(VERSION 3.28)
project(zydis LANGUAGES C)

# Zydis: the x86-64 instruction decoder behind tools/instruction-tracer. Not committed — its
# amalgamated Zydis.c is ~12 MB of generated tables — so fetch-zydis.py generates it into
# .install/ on demand (dev.py runs it per configure; see tools/dev/lib/pipeline/prereqs.py).
# Here we build that single TU into a static lib, the same shape as the vendored xxHash.
set(ZYDIS_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${ZYDIS_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "Zydis is not installed (${ZYDIS_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/zydis/fetch-zydis.py\n"
        "dev.py does this automatically on Windows; see tools/dev/lib/pipeline/prereqs.py.")
endif()

add_library(zydis STATIC ${ZYDIS_INSTALL}/src/Zydis.c)

# SYSTEM so Zydis.h does not trip the consuming TU's warnings.
target_include_directories(zydis SYSTEM PUBLIC ${ZYDIS_INSTALL}/include)

# Both are required and PUBLIC: without them Zydis.h declares its API dllimport (it assumes a
# shared build), which both warns on definition and breaks the static link. Note there is no
# ZYDIS_DISABLE_ENCODER here on purpose — amalgamate.py merges Encoder.c unconditionally while
# leaving the header's encoder decls guarded, so defining it fails to compile.
target_compile_definitions(zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(zydis PRIVATE /W0)
else()
    target_compile_options(zydis PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET zydis PROPERTY POSITION_INDEPENDENT_CODE ON)
