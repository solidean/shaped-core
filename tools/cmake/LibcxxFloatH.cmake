# Works around libc++ 22's <float.h> against the macOS 27 SDK, by putting tools/cmake/libcxx-float-h ahead of it on the C++ include path.
# The header itself explains the mechanism; docs/requirements.md has the symptom and when this goes away.
#
# Only Homebrew LLVM below 23 on Apple needs it.
# AppleClang ships a libc++ without <float.h>, and LLVM 23 removed it upstream (llvm-project #175849), so neither has the guard to reopen.
# Against the macOS 26 SDK the shim is inert: nothing includes <float.h> partially there, so the guard it reopens is never closed early.
#
# C is untouched, since it has no libc++ on its include path and reaches clang's <float.h> directly.
# Goes through sc_build_options so vendored code (imgui includes <float.h>) and consumers of shaped-core both get it.

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 23)
    sc_build_options(COMPILE "$<$<COMPILE_LANGUAGE:CXX>:-isystem${CMAKE_CURRENT_LIST_DIR}/libcxx-float-h>")
endif()
