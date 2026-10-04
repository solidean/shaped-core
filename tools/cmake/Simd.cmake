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

# What clean-simd's cimd_dispatch reads: the kernels compiled above the floor, and each one's flags and x86-64 level.
# GLOBAL properties rather than variables, so a cimd_dispatch outside this directory tree sees them too — the case when
# shaped-core is consumed with add_subdirectory.
# Empty off x86-64: neon and simd128 are the floor there, and nothing sits above them.
set(SC_SIMD_DISPATCH_KERNELS "")
set_property(GLOBAL PROPERTY SC_SIMD_DISPATCH_KERNELS "")

if(NOT SC_ARCH_X64)
    if(SC_SIMD_KERNELS AND NOT SC_SIMD_KERNELS STREQUAL "auto")
        message(FATAL_ERROR "SC_SIMD_KERNELS names x86-64 kernels ('${SC_SIMD_KERNELS}'), but this target is not x86-64.")
    endif()
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
    # compare into a k-register and back.
    # v4 therefore builds as /arch:AVX2, and the avx512 kernel stays a dispatched one: its TU alone gets /arch:AVX512,
    # which is what defines the __AVX512*__ macros clean-simd's kernel.hh requires.
    if(_sc_x64_level STREQUAL "v2")
        add_compile_options(/arch:SSE4.2)
    elseif(_sc_x64_level STREQUAL "v3" OR _sc_x64_level STREQUAL "v4")
        add_compile_options(/arch:AVX2)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-march=x86-64-${_sc_x64_level})
endif()

add_compile_definitions(CC_X64_LEVEL=${_sc_x64_level_number})

# The kernel the floor compiles everywhere: one per level, except that cl.exe builds v4 as AVX2 (above).
set(_sc_kernel_order sse2 sse42 avx2 avx512)
set(_sc_floor_index ${_sc_x64_level_number})
math(EXPR _sc_floor_index "${_sc_floor_index} - 1")
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND _sc_floor_index GREATER 2)
    set(_sc_floor_index 2)
endif()

set(_sc_above "")
foreach(_k IN LISTS _sc_kernel_order)
    list(FIND _sc_kernel_order ${_k} _i)
    if(_i GREATER _sc_floor_index)
        list(APPEND _sc_above ${_k})
    endif()
endforeach()

if(SC_SIMD_KERNELS STREQUAL "auto")
    set(SC_SIMD_DISPATCH_KERNELS ${_sc_above})
else()
    foreach(_k IN LISTS SC_SIMD_KERNELS)
        if(NOT _k IN_LIST _sc_above)
            message(FATAL_ERROR "SC_SIMD_KERNELS: '${_k}' is not a kernel above the SC_X64_LEVEL floor "
                                "(${_sc_x64_level}); those are: ${_sc_above}")
        endif()
    endforeach()
    # Weakest first whatever order the list was given in: a dispatch table's last supported row is its best.
    foreach(_k IN LISTS _sc_kernel_order)
        if(_k IN_LIST SC_SIMD_KERNELS)
            list(APPEND SC_SIMD_DISPATCH_KERNELS ${_k})
        endif()
    endforeach()
endif()
set_property(GLOBAL PROPERTY SC_SIMD_DISPATCH_KERNELS "${SC_SIMD_DISPATCH_KERNELS}")

# Each dispatched kernel's flags, spelled the way this compiler takes a level.
set(_sc_cl_arch_sse42 "/arch:SSE4.2")
set(_sc_cl_arch_avx2 "/arch:AVX2")
set(_sc_cl_arch_avx512 "/arch:AVX512")
foreach(_k IN ITEMS sse42 avx2 avx512)
    list(FIND _sc_kernel_order ${_k} _i)
    math(EXPR _level "${_i} + 1")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
        set(SC_SIMD_KERNEL_FLAGS_${_k} "/clang:-march=x86-64-v${_level}")
    elseif(MSVC)
        set(SC_SIMD_KERNEL_FLAGS_${_k} "${_sc_cl_arch_${_k}}")
    else()
        set(SC_SIMD_KERNEL_FLAGS_${_k} "-march=x86-64-v${_level}")
    endif()
    set_property(GLOBAL PROPERTY SC_SIMD_KERNEL_FLAGS_${_k} "${SC_SIMD_KERNEL_FLAGS_${_k}}")
    set_property(GLOBAL PROPERTY SC_SIMD_KERNEL_LEVEL_${_k} ${_level})
endforeach()
