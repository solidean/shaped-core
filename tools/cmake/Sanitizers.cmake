# Sanitizer wiring (repo-wide). Included once from the root CMakeLists before
# add_subdirectory so every target is instrumented. A no-op unless SANITIZE is
# set (a comma-separated list, e.g. "address,undefined"), driven by the
# *-sanitize-* presets. Clang/AppleClang/clang-cl only.

set(SANITIZE "" CACHE STRING "Comma-separated sanitizers to enable (e.g. address,undefined)")
if(SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SANITIZE requires a Clang/LLVM toolchain (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
    add_compile_options(-fsanitize=${SANITIZE})

    if(NOT MSVC)
        # Unix clang/AppleClang: the compiler driver also links, so it wires the
        # sanitizer runtime itself. -fno-omit-frame-pointer keeps traces readable.
        add_compile_options(-fno-omit-frame-pointer)
        add_link_options(-fsanitize=${SANITIZE})
    else()
        # clang-cl. ASan is incompatible with the debug CRT (/MDd) the Debug build
        # type selects; force the non-debug dynamic CRT — also the only one the
        # Windows LLVM toolchain ships an ASan runtime for. /Oy- keeps frame
        # pointers (clang-cl rejects -fno-omit-frame-pointer).
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
        add_compile_options(/Oy-)

        # CMake links clang-cl targets with lld-link directly, bypassing the driver
        # that would add the sanitizer runtime — so add it ourselves, mirroring
        # `clang-cl -fsanitize=address -###`. In combined address+undefined mode
        # UBSan routes through the ASan runtime, so the ASan libs suffice.
        if(NOT SANITIZE MATCHES "address")
            message(FATAL_ERROR "SANITIZE='${SANITIZE}' on clang-cl: only address[,undefined] is wired for Windows")
        endif()
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                        OUTPUT_VARIABLE _sc_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(SC_ASAN_RUNTIME_DIR "${_sc_resource_dir}/lib/windows" CACHE INTERNAL "clang_rt dir")
        add_link_options(
            "-libpath:${SC_ASAN_RUNTIME_DIR}"
            "${SC_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.lib"
            "-wholearchive:${SC_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
            "-include:__asan_seh_interceptor"
            "/INFERASANLIBS:NO"
        )
    endif()
endif()
