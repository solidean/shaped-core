# Vendored third-party dependencies.
# Each lives in-tree under extern/<lib>/ at a pinned commit, declared in that directory's dependency.yml.
# `uv run dev.py deps list` shows every pin and whether upstream has moved on; the per-lib vendor-*.py / fetch-*.py reads its pin from the same manifest.
# Each SC_USE_VENDORED_<LIB> defaults ON and builds the in-tree copy;
# turning one OFF means the embedding project must already provide a target of that name.
#
# Add new vendored deps here so the root CMakeLists.txt only does add_subdirectory(extern).

option(SC_USE_VENDORED_MIMALLOC "Build the in-tree vendored mimalloc (OFF expects an external `mimalloc` target)" ON)
option(SC_USE_VENDORED_XXHASH "Build the in-tree vendored xxHash (OFF expects an external `xxhash` target)" ON)

# BLAKE3's C implementation, committed in-source (a handful of small TUs; the hand-written assembly variants are not vendored).
# Backs cc::blake3 / cc::hash256, the cryptographic hash content addressing needs; linked PRIVATE by clean-core, so blake3.h reaches one TU.
# Vendored rather than fetched because it is small and every build needs it — the xxHash tier, not the SQLite one.
option(SC_USE_VENDORED_BLAKE3 "Build the in-tree vendored BLAKE3 (OFF expects an external `blake3` target)" ON)

# Zstandard and LZ4, committed in-source: together they are cc::compress / cc::decompress.
# Linked PRIVATE by clean-core, so neither upstream header reaches a cc public header.
# Vendored rather than fetched because compression is not optional — a format that cannot decode its own blobs
# when a flag is off is a broken format, so there is no configuration in which these may be absent.
# zstd is the largest thing vendored in source here (~2.1 MB), which is the price of that guarantee;
# upstream's legacy decoders, deprecated zbuff API and DLL projects are not part of it.
option(SC_USE_VENDORED_ZSTD "Build the in-tree vendored Zstandard (OFF expects an external `zstd` target)" ON)
option(SC_USE_VENDORED_LZ4 "Build the in-tree vendored LZ4 (OFF expects an external `lz4` target)" ON)

# zlib, committed in-source: the Deflate third of cc::compress / cc::decompress, and the same always-present argument applies.
# Deflate is the interop codec — what reads a zip, a gzip or a PNG stream — while zstd and lz4 own the ratio and speed axes.
# zlib rather than the faster zlib-ng because it needs no generated config and no per-architecture SIMD detection,
# so it compiles unchanged on every target; the vendored subset drops the stdio gzFile layer, which cc::read_stream replaces.
option(SC_USE_VENDORED_ZLIB "Build the in-tree vendored zlib (OFF expects an external `zlib` target)" ON)

# stb single-file image libraries (stb_image + stb_image_write), committed in-source (two small headers).
# Back babel-serializer's image codecs; linked PRIVATE there, so no stb header ever reaches a babel public header.
option(SC_USE_VENDORED_STB "Build the in-tree vendored stb (OFF expects an external `stb` target)" ON)

# libspng, committed in-source: two files, and the PNG half of babel-serializer's image codecs.
# Two files with no build machinery worth keeping puts it in the xxhash tier, which is why it is in-source rather than fetched.
# Linked PRIVATE by babel, so spng.h never reaches a babel public header; it decodes over the vendored zlib above rather than a second bundled Deflate.
option(SC_USE_VENDORED_SPNG "Build the in-tree vendored libspng (OFF expects an external `spng` target)" ON)

# Dear ImGui, tracked on the docking branch.
# Consumed by shaped-rendering, which draws it through sg —
# upstream's own per-API backends are not vendored, so nothing here pulls in native graphics calls.
option(SC_USE_VENDORED_IMGUI "Build the in-tree vendored Dear ImGui (OFF expects an external `imgui` target)" ON)

# DXC is built-from-source (not vendored as source) and only consumed by the Windows-only shaped-shader-compiler-dxc library.
# Wire it only when it has actually been built, so a plain checkout still configures;
# the graphics library gates itself on the `dxc` target existing.
option(SC_USE_VENDORED_DXC "Wrap the built-from-source DXC in extern/dxc/.install (OFF expects an external `dxc` target)" ON)

# Zydis is generated-on-demand rather than vendored as source (its amalgamated TU is ~12 MB of instruction tables) and is only consumed by the optional, Windows-only instruction-tracer tool.
# Wire it only when it has actually been fetched, so a plain checkout still configures;
# the tool gates itself on the `zydis` target existing.
# SC_ARCH_X64 keeps that 12 MB TU off ARM64 builds, where it decodes an instruction set nothing present can execute.
option(SC_USE_VENDORED_ZYDIS "Build the fetched Zydis in extern/zydis/.install (OFF expects an external `zydis` target)" ON)

# SDL3 is fetched-on-demand rather than vendored as source.
# Its tree is ~50 MB across several thousand files.
# It backs shaped-rendering's sr::window, and unlike DXC and Zydis it is wanted on every platform.
# Wire it only when it has actually been fetched, so a plain checkout still configures.
# shaped-rendering gates its window backend on the SDL3::SDL3 target existing.
option(SC_USE_VENDORED_SDL3 "Build the fetched SDL3 in extern/sdl3/.install (OFF expects an external `SDL3::SDL3` target)" ON)

# SQLite is fetched-on-demand rather than vendored as source (its amalgamated sqlite3.c is ~9.5 MB of generated C) and, like SDL3, is wanted on every platform.
# It backs babel-serializer's babel::sqlite format.
# Wire it only when it has actually been fetched, so a plain checkout still configures;
# babel-serializer keeps its sqlite API always available and reports an unfetched backend at runtime.
option(SC_USE_VENDORED_SQLITE "Build the fetched SQLite in extern/sqlite/.install (OFF expects an external `sqlite` target)" ON)

# Also gated on SC_MIMALLOC: with the default resource on operator new nothing links mimalloc, so building it would be
# dead weight in exactly the sanitizer builds that are already the slowest.
if(SC_USE_VENDORED_MIMALLOC AND SC_MIMALLOC)
    add_subdirectory(mimalloc)
endif()

if(SC_USE_VENDORED_XXHASH)
    add_subdirectory(xxhash)
endif()

if(SC_USE_VENDORED_BLAKE3)
    add_subdirectory(blake3)
endif()

if(SC_USE_VENDORED_ZSTD)
    add_subdirectory(zstd)
endif()

if(SC_USE_VENDORED_LZ4)
    add_subdirectory(lz4)
endif()

if(SC_USE_VENDORED_ZLIB)
    add_subdirectory(zlib)
endif()

if(SC_USE_VENDORED_STB)
    add_subdirectory(stb)
endif()

if(SC_USE_VENDORED_SPNG)
    add_subdirectory(libspng)
endif()

if(SC_USE_VENDORED_IMGUI)
    add_subdirectory(imgui)
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

if(SC_USE_VENDORED_SQLITE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sqlite/.install/pin.txt")
    add_subdirectory(sqlite)
endif()
