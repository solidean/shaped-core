# Vendored third-party dependencies. Each lives in-tree under extern/<lib>/ at a
# pinned commit (see the per-lib vendor-*.py). Each SC_USE_VENDORED_<LIB> defaults
# ON and builds the in-tree copy; turning one OFF means the embedding project must
# already provide a target of that name.
#
# Add new vendored deps here so the root CMakeLists.txt only does add_subdirectory(extern).

option(SC_USE_VENDORED_MIMALLOC "Build the in-tree vendored mimalloc (OFF expects an external `mimalloc` target)" ON)
option(SC_USE_VENDORED_XXHASH "Build the in-tree vendored xxHash (OFF expects an external `xxhash` target)" ON)

# DXC is built-from-source (not vendored as source) and only consumed by the Windows-only
# shaped-shader-compiler-dxc library. Wire it only when it has actually been built, so a plain
# checkout still configures; the graphics library gates itself on the `dxc` target existing.
option(SC_USE_VENDORED_DXC "Wrap the built-from-source DXC in extern/dxc/.install (OFF expects an external `dxc` target)" ON)

# Zydis is generated-on-demand rather than vendored as source (its amalgamated TU is ~12 MB of
# instruction tables) and is only consumed by the optional, Windows-only instruction-tracer tool.
# Wire it only when it has actually been fetched, so a plain checkout still configures; the tool
# gates itself on the `zydis` target existing. SC_ARCH_X64 keeps that 12 MB TU off ARM64 builds,
# where it decodes an instruction set nothing present can execute.
option(SC_USE_VENDORED_ZYDIS "Build the fetched Zydis in extern/zydis/.install (OFF expects an external `zydis` target)" ON)

# SDL3 is fetched-on-demand rather than vendored as source.
# Its tree is ~50 MB across several thousand files.
# It backs shaped-rendering's sr::window, and unlike DXC and Zydis it is wanted on every platform.
# Wire it only when it has actually been fetched, so a plain checkout still configures.
# shaped-rendering gates its window backend on the SDL3::SDL3 target existing.
option(SC_USE_VENDORED_SDL3 "Build the fetched SDL3 in extern/sdl3/.install (OFF expects an external `SDL3::SDL3` target)" ON)

if(SC_USE_VENDORED_MIMALLOC)
    add_subdirectory(mimalloc)
endif()

if(SC_USE_VENDORED_XXHASH)
    add_subdirectory(xxhash)
endif()

if(SC_USE_VENDORED_DXC AND WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dxc/.install/pin.txt")
    add_subdirectory(dxc)
endif()

if(SC_USE_VENDORED_ZYDIS AND SC_BUILD_TOOLS AND SC_ARCH_X64 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zydis/.install/pin.txt")
    add_subdirectory(zydis)
endif()

if(SC_USE_VENDORED_SDL3 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdl3/.install/pin.txt")
    add_subdirectory(sdl3)
endif()
