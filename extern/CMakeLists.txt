# Vendored third-party dependencies.
# Each lives in-tree under extern/<lib>/ at a pinned commit, declared in that directory's dependency.yml.
# `uv run dev.py deps list` shows every pin and whether upstream has moved on; the per-lib vendor-*.py / fetch-*.py reads its pin from the same manifest.
# Each SC_USE_VENDORED_<LIB> defaults ON and builds the in-tree copy;
# turning one OFF means the embedding project must already provide a target of that name.
# clean-core's six (mimalloc, xxHash, BLAKE3, zstd, LZ4, zlib) are found by find_package where no such target exists,
# under the package and target names Conan's CMakeDeps and most package managers give them; see _sc_external_package below.
#
# Under a SC_LIBRARIES subset every directory here is added EXCLUDE_FROM_ALL: configured, so a built library finds its
# target, and built only where a built library links it — an external nothing built needs is neither built nor installed.
#
# Add new vendored deps here so the root CMakeLists.txt only does add_subdirectory(extern).

option(SC_USE_VENDORED_MIMALLOC "Build the in-tree vendored mimalloc (OFF expects an external `mimalloc` target, or finds package mimalloc)" ON)
option(SC_USE_VENDORED_XXHASH "Build the in-tree vendored xxHash (OFF expects an external `xxhash` target, or finds package xxHash)" ON)

# BLAKE3's C implementation, committed in-source (a handful of small TUs; the hand-written assembly variants are not vendored).
# Backs cc::blake3 / cc::hash256, the cryptographic hash content addressing needs; linked PRIVATE by clean-core, so blake3.h reaches one TU.
# Vendored rather than fetched because it is small and every build needs it — the xxHash tier, not the SQLite one.
option(SC_USE_VENDORED_BLAKE3 "Build the in-tree vendored BLAKE3 (OFF expects an external `blake3` target, or finds package BLAKE3)" ON)

# Zstandard and LZ4, committed in-source: together they are cc::compress / cc::decompress.
# Linked PRIVATE by clean-core, so neither upstream header reaches a cc public header.
# Vendored rather than fetched because compression is not optional — a format that cannot decode its own blobs
# when a flag is off is a broken format, so there is no configuration in which these may be absent.
# zstd is the largest thing vendored in source here (~2.1 MB), which is the price of that guarantee;
# upstream's legacy decoders, deprecated zbuff API and DLL projects are not part of it.
option(SC_USE_VENDORED_ZSTD "Build the in-tree vendored Zstandard (OFF expects an external `zstd` target, or finds package zstd)" ON)
option(SC_USE_VENDORED_LZ4 "Build the in-tree vendored LZ4 (OFF expects an external `lz4` target, or finds package lz4)" ON)

# zlib, committed in-source: the Deflate third of cc::compress / cc::decompress, and the same always-present argument applies.
# Deflate is the interop codec — what reads a zip, a gzip or a PNG stream — while zstd and lz4 own the ratio and speed axes.
# zlib rather than the faster zlib-ng because it needs no generated config and no per-architecture SIMD detection,
# so it compiles unchanged on every target; the vendored subset drops the stdio gzFile layer, which cc::read_stream replaces.
option(SC_USE_VENDORED_ZLIB "Build the in-tree vendored zlib (OFF expects an external `zlib` target, or finds package ZLIB)" ON)

# stb single-file image libraries (stb_image + stb_image_write), committed in-source (two small headers).
# Back babel-serializer's image codecs; linked PRIVATE there, so no stb header ever reaches a babel public header.
option(SC_USE_VENDORED_STB "Build the in-tree vendored stb (OFF expects an external `stb` target)" ON)

# libspng, committed in-source: two files, and the PNG half of babel-serializer's image codecs.
# Two files with no build machinery worth keeping puts it in the xxhash tier, which is why it is in-source rather than fetched.
# Linked PRIVATE by babel, so spng.h never reaches a babel public header; it decodes over the vendored zlib above rather than a second bundled Deflate.
option(SC_USE_VENDORED_SPNG "Build the in-tree vendored libspng (OFF expects an external `spng` target)" ON)

# libwebp, committed in-source: the still-image codec (decode, encode, dsp, sharpyuv), and the WebP half of babel-serializer's image codecs.
# Vendored rather than fetched because it is plain C with no generated config, so it builds unchanged on every target.
# Linked PRIVATE by babel, so webp's headers never reach a babel public header.
option(SC_USE_VENDORED_LIBWEBP "Build the in-tree vendored libwebp (OFF expects an external `webp` target)" ON)

# ufbx, committed in-source: one C file and one header, and the whole of babel-serializer's FBX reader.
# It sits in the libspng tier for the same reason: two files, no build machinery and no dependencies, not even a Deflate.
# Linked PRIVATE by babel, so ufbx.h never reaches a babel public header.
option(SC_USE_VENDORED_UFBX "Build the in-tree vendored ufbx (OFF expects an external `ufbx` target)" ON)

# meshoptimizer's decoders, committed in-source: the vertex and index codecs and the vertex filters, three files and one header.
# They are babel-serializer's EXT_meshopt_compression, the glTF extension that packs vertex and index buffers.
# The libspng tier again: a few files with no build machinery and no dependencies; linked PRIVATE by babel.
option(SC_USE_VENDORED_MESHOPTIMIZER "Build the in-tree vendored meshoptimizer decoders (OFF expects an external `meshoptimizer` target)" ON)

# Mbed TLS, committed in-source: the TLS handshake and record layer behind clean-net's TLS connections.
# Vendored rather than fetched because a build that cannot do HTTPS is not a working build of clean-net -- the same
# always-present argument zstd and zlib make, and the reason there is no configuration in which this may be absent.
# It is the largest thing vendored in source here (~8 MB across 272 files), which is the price of that guarantee.
# Mbed TLS rather than OpenSSL or BoringSSL: plain CMake, no external toolchain, and it builds unchanged on all eight
# platform/compiler combinations CI covers -- three of which (Linux, Android, wasm) have no system TLS API at all.
option(SC_USE_VENDORED_MBEDTLS "Build the in-tree vendored Mbed TLS (OFF expects an external `mbedtls` target)" ON)

# SPIRV-Reflect, committed in-source (three files).
# Backs shaped-shader-compiler-dxc's SPIR-V reflection: a module carries its bindings in its own instruction stream, so
# reading them back is a parser rather than a query, and DXC emits no reflection container for a -spirv compile.
option(SC_USE_VENDORED_SPIRV_REFLECT "Build the in-tree vendored SPIRV-Reflect (OFF expects an external `spirv-reflect` target)" ON)

# metal-cpp, Apple's header-only C++ interface to Metal, Foundation and QuartzCore.
# The whole of shaped-graphics-metal is written against it, which is what keeps Objective-C++ out of the tree —
# our format, lint and PCH tooling all speak only ('.cc', '.hh'), so a `.mm` source would mean editing the dev driver first.
# Apple-only by construction, so the add_subdirectory below is gated rather than this option.
option(SC_USE_VENDORED_METAL_CPP "Build the in-tree vendored metal-cpp (OFF expects an external `metal-cpp` target)" ON)

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

# DLSS is fetched ON REQUEST rather than on demand: no configure step fetches it, because its license is NVIDIA's own
# and accepting one is a person's decision rather than a build's.
# So the normal state of this directory is absent, and that is not a failure — shaped-rendering gates its Ray
# Reconstruction member on the `dlss` target existing and reports `unsupported` without it.
option(SC_USE_VENDORED_DLSS "Wrap the fetched NVIDIA DLSS SDK in extern/dlss/.install (OFF expects an external `dlss` target)" ON)

# NRD is fetched ON REQUEST like DLSS, and for the same reason: the NVIDIA RTX SDKs license names the NRD SDK too.
# Unlike DLSS it is SOURCE, and its build compiles its own shaders with the dxc.exe extern/dxc pins.
option(SC_USE_VENDORED_NRD "Build the fetched NRD in extern/nrd/.install (OFF expects an external `NRD` target)" ON)

# WARP from NuGet is fetched ON REQUEST like DLSS: its terms are testing-only and forbid redistributing it.
# Nothing links it; the directory only publishes where the DLL is, for shaped-graphics to stage beside its WARP test binaries.
option(SC_USE_VENDORED_WARP "Stage the fetched WARP in extern/warp/.install beside the dx12 test binaries" ON)

# The D3D12 Agility SDK from NuGet is fetched ON REQUEST, and with `--accept-license` only: its terms are Microsoft's
# DirectX license. Without it the dx12 backend runs on the system runtime; extern/agility/dependency.yml says what it adds.
option(SC_USE_VENDORED_AGILITY "Create dx12 devices through the fetched Agility SDK in extern/agility/.install" ON)

# The OIDN library is only the reference shaped-rendering-test's oracle compares sr::denoise_method::oidn against, and nothing we ship links it.
# So it is fetched ON REQUEST — `uv run extern/oidn/fetch-oidn.py` — rather than on every configure, and the oracle tests SKIP without the `oidn` target.
option(SC_USE_VENDORED_OIDN "Wrap the fetched Open Image Denoise in extern/oidn/.install (OFF expects an external `oidn` target)" ON)

# The trained networks, a dependency of their own because shaped-rendering runs them itself rather than calling Intel's inference.
# Two files and 2.5 MB of weights instead of 53 MB of runtime, fetched on every configure since the member cannot run without them.
option(SC_USE_VENDORED_OIDN_WEIGHTS "Use the fetched OIDN weights in extern/oidn-weights/.install" ON)

# AMD FSR 3.1's upscaler sources: its host code, which shaped-rendering drives through its own backend over sg, and the
# HLSL passes that backend compiles.
# Fetched on every configure on Windows, since the upscaler routine has nothing to run without them; the host code is
# not built against another C runtime yet, so elsewhere sr builds without FSR.
option(SC_USE_VENDORED_FIDELITYFX "Build the fetched FSR 3.1 sources in extern/fidelityfx/.install" ON)

if(SC_LIBRARY_SUBSET)
    set(_sc_extern_scope EXCLUDE_FROM_ALL)
else()
    set(_sc_extern_scope "")
endif()

# _sc_external_package(<target> <option> <package> <imported target>...)
# A non-vendored clean-core external: an existing <target> is used as it is, as every SC_USE_VENDORED_<LIB>=OFF is.
# Otherwise find_package(<package>) runs, and <target> becomes an alias of the first of the imported targets it defined.
# GLOBAL, since clean-core links the alias from another directory.
# The package and its target are recorded for tools/cmake/Install.cmake: shaped-coreConfig.cmake finds the package again.
function(_sc_external_package target option package)
    if(TARGET ${target})
        return()
    endif()
    find_package(${package} GLOBAL)
    foreach(_imported IN ITEMS ${target} ${ARGN}) # mimalloc's own config names its target as we do
        if(TARGET ${_imported})
            if(NOT _imported STREQUAL target)
                add_library(${target} ALIAS ${_imported})
            endif()
            set_property(GLOBAL APPEND PROPERTY SC_EXTERNAL_PACKAGES ${package})
            set_property(GLOBAL APPEND PROPERTY SC_EXTERNAL_PACKAGE_TARGETS ${_imported})
            return()
        endif()
    endforeach()
    list(JOIN ARGN ", " _candidates)
    message(FATAL_ERROR "${option} is OFF, but no `${target}` target exists and find_package(${package}) defined none of "
                        "${_candidates}.
Provide the package, or turn ${option} back on to build the vendored copy.")
endfunction()

# Also gated on SC_MIMALLOC: with the default resource on operator new nothing links mimalloc, so building it would be
# dead weight in exactly the sanitizer builds that are already the slowest.
if(SC_MIMALLOC)
    if(SC_USE_VENDORED_MIMALLOC)
        add_subdirectory(mimalloc ${_sc_extern_scope})
    else()
        _sc_external_package(mimalloc SC_USE_VENDORED_MIMALLOC mimalloc mimalloc-static)
    endif()
endif()

if(SC_USE_VENDORED_XXHASH)
    add_subdirectory(xxhash ${_sc_extern_scope})
else()
    _sc_external_package(xxhash SC_USE_VENDORED_XXHASH xxHash xxHash::xxhash)
endif()

if(SC_USE_VENDORED_BLAKE3)
    add_subdirectory(blake3 ${_sc_extern_scope})
else()
    _sc_external_package(blake3 SC_USE_VENDORED_BLAKE3 BLAKE3 BLAKE3::blake3)
endif()

if(SC_USE_VENDORED_ZSTD)
    add_subdirectory(zstd ${_sc_extern_scope})
else()
    _sc_external_package(zstd SC_USE_VENDORED_ZSTD zstd zstd::libzstd_static zstd::libzstd_shared zstd::libzstd)
endif()

if(SC_USE_VENDORED_LZ4)
    add_subdirectory(lz4 ${_sc_extern_scope})
else()
    _sc_external_package(lz4 SC_USE_VENDORED_LZ4 lz4 lz4::lz4 LZ4::lz4 LZ4::lz4_static LZ4::lz4_shared)
endif()

if(SC_USE_VENDORED_ZLIB)
    add_subdirectory(zlib ${_sc_extern_scope})
else()
    _sc_external_package(zlib SC_USE_VENDORED_ZLIB ZLIB ZLIB::ZLIB)
endif()

if(SC_USE_VENDORED_STB)
    add_subdirectory(stb ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_SPNG)
    add_subdirectory(libspng ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_LIBWEBP)
    add_subdirectory(libwebp ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_UFBX)
    add_subdirectory(ufbx ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_MESHOPTIMIZER)
    add_subdirectory(meshoptimizer ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_MBEDTLS)
    add_subdirectory(mbedtls ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_SPIRV_REFLECT)
    add_subdirectory(spirv-reflect ${_sc_extern_scope})
endif()

if(SC_USE_VENDORED_IMGUI)
    add_subdirectory(imgui ${_sc_extern_scope})
endif()

# Inter, the UI font a web page draws text in; one vendored file, built into nothing.
add_subdirectory(inter)

# APPLE covers macOS and iOS alike, which is the platform set metal-cpp compiles on at all.
# The metal backend gates itself on the target existing, so a non-Apple build simply never sees one.
if(SC_USE_VENDORED_METAL_CPP AND APPLE)
    add_subdirectory(metal-cpp ${_sc_extern_scope})
endif()

# The fetched deps below are host-side, so a cross-target build must not pick them up.
# dev.py already declines to fetch them for such a preset (_NON_NATIVE in tools/dev/lib/pipeline/prereqs.py);
# the pin.txt guards only see whether some *earlier native* configure left an install behind, which on a developer
# machine it has. Without this a wasm build links host binaries CI never sees -- a prebuilt libdxcompiler, an SDL
# that has no shared linkage here -- so the same tree builds on CI and fails on a workstation.
# Named platforms rather than CMAKE_CROSSCOMPILING, which would also strip an arm64 cross build that wants them.
# Lower-case and underscore-prefixed because it is DERIVED rather than settable: every SC_* name in this repo is a
# knob a caller may set, and this one is not.
# A new cross target belongs in the list below -- WASI is the next one due, and it fails the same silent way.
set(_sc_host_side_deps ON)
if(EMSCRIPTEN OR ANDROID OR IOS)
    set(_sc_host_side_deps OFF)
endif()

# A subdirectory that is not added must not leave the CACHE INTERNAL values an earlier configure set behind.
# Otherwise switching an option off, or deleting an .install/, keeps consumers staging and compiling in paths that are gone.
if(_sc_host_side_deps AND SC_USE_VENDORED_DXC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dxc/.install/pin.txt")
    add_subdirectory(dxc ${_sc_extern_scope})
else()
    unset(DXC_RUNTIME_DLLS CACHE)
endif()

if(_sc_host_side_deps AND SC_USE_VENDORED_ZYDIS AND SC_BUILD_TOOLS AND SC_ARCH_X64 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zydis/.install/pin.txt")
    add_subdirectory(zydis ${_sc_extern_scope})
endif()

if(_sc_host_side_deps AND SC_USE_VENDORED_SDL3 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdl3/.install/pin.txt")
    add_subdirectory(sdl3 ${_sc_extern_scope})
endif()

# The OpenXR loader, prebuilt for Windows; it defines `openxr_loader` where it was fetched.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/openxr/.install/pin.txt")
    add_subdirectory(openxr ${_sc_extern_scope})
endif()

if(_sc_host_side_deps AND SC_USE_VENDORED_SQLITE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sqlite/.install/pin.txt")
    add_subdirectory(sqlite ${_sc_extern_scope})
endif()

# After dxc, whose .install supplies the compiler NRD's shader build runs.
if(_sc_host_side_deps AND SC_USE_VENDORED_NRD AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nrd/.install/pin.txt")
    add_subdirectory(nrd ${_sc_extern_scope})
endif()

if(_sc_host_side_deps AND SC_USE_VENDORED_OIDN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/oidn/.install/pin.txt")
    add_subdirectory(oidn ${_sc_extern_scope})
else()
    unset(OIDN_RUNTIME_LIBRARIES CACHE)
endif()

if(_sc_host_side_deps AND SC_USE_VENDORED_OIDN_WEIGHTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/oidn-weights/.install/pin.txt")
    add_subdirectory(oidn-weights ${_sc_extern_scope})
else()
    unset(OIDN_WEIGHTS_DIR CACHE)
endif()

if(_sc_host_side_deps AND WIN32 AND SC_USE_VENDORED_FIDELITYFX
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fidelityfx/.install/pin.txt")
    add_subdirectory(fidelityfx ${_sc_extern_scope})
endif()

if(_sc_host_side_deps AND SC_USE_VENDORED_DLSS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dlss/.install/pin.txt")
    add_subdirectory(dlss ${_sc_extern_scope})
endif()

# Reset first, so uninstalling it takes effect at the next configure rather than leaving the old path cached.
set(SC_PINNED_WARP_DLL "" CACHE INTERNAL "The pinned WARP the dx12 test binaries load")
if(WIN32 AND SC_USE_VENDORED_WARP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/warp/.install/pin.txt")
    add_subdirectory(warp ${_sc_extern_scope})
endif()

# SC_AGILITY_PREVIEW swaps the runtime for extern/agility-preview's pin, read through the same wrapper.
# It is required there, since a retail build under that name would test nothing the switch is for.
set(_sc_agility_install "${CMAKE_CURRENT_SOURCE_DIR}/agility/.install")
if(WIN32 AND SC_AGILITY_PREVIEW)
    set(_sc_agility_install "${CMAKE_CURRENT_SOURCE_DIR}/agility-preview/.install")
    if(NOT SC_USE_VENDORED_AGILITY OR NOT EXISTS "${_sc_agility_install}/pin.txt")
        message(FATAL_ERROR "SC_AGILITY_PREVIEW is ON, but no preview Agility SDK was fetched: "
                            "run `uv run extern/agility/fetch-agility.py --preview --accept-license` after reading its terms")
    endif()
endif()

if(WIN32 AND SC_USE_VENDORED_AGILITY AND EXISTS "${_sc_agility_install}/pin.txt")
    set(AGILITY_INSTALL "${_sc_agility_install}")
    add_subdirectory(agility ${_sc_extern_scope})
endif()
