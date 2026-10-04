# The x86-64 feature level every function in the build may assume (repo-wide). Included once from the root
# CMakeLists before add_subdirectory(extern); a no-op on targets that are not x86-64.
#
# SC_X64_LEVEL is the floor, not the ceiling: the compiler spends the flag in every function it emits, so a binary
# built at a level runs only on CPUs at that level or above.
# Code that wants more than the floor reaches it through clean-simd's dispatched kernels, which compile only their own
# TUs above it — see libs/base/clean-simd/docs/design.md.
#
# The level also reaches the compiler as CC_X64_LEVEL=<n>, because MSVC reports __AVX2__ but none of the SSE levels,
# so a header cannot recover v2 from the predefined macros alone.
#
# A whole-build switch, never per-target: an inline function compiled at two levels is an ODR violation the linker
# resolves silently, by keeping whichever copy it saw first.

if(NOT SC_ARCH_X64)
    return()
endif()

string(TOLOWER "${SC_X64_LEVEL}" _sc_x64_level)
if(NOT _sc_x64_level MATCHES "^v[1-4]$")
    message(FATAL_ERROR "SC_X64_LEVEL must be one of v1, v2, v3, v4 (got '${SC_X64_LEVEL}').")
endif()
string(SUBSTRING "${_sc_x64_level}" 1 1 _sc_x64_level_number)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
    # clang-cl's /arch: spellings imply a different feature set than the x86-64-vN levels, so pass the level itself.
    if(NOT _sc_x64_level STREQUAL "v1")
        add_compile_options("/clang:-march=x86-64-${_sc_x64_level}")
    endif()
elseif(MSVC)
    # cl.exe has no v4 spelling that keeps its AVX2-tier code at AVX2 quality: with /arch:AVX512 it moves every vector
    # compare into a k-register and back. v4 therefore builds as /arch:AVX2; clean-simd's avx512 kernel uses
    # k-register intrinsics directly and needs no flag on cl.exe.
    if(_sc_x64_level STREQUAL "v2")
        add_compile_options(/arch:SSE4.2)
    elseif(_sc_x64_level STREQUAL "v3" OR _sc_x64_level STREQUAL "v4")
        add_compile_options(/arch:AVX2)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-march=x86-64-${_sc_x64_level})
endif()

add_compile_definitions(CC_X64_LEVEL=${_sc_x64_level_number})
