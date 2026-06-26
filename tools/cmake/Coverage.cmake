# LLVM source-based test coverage (repo-wide). Included once from the root
# CMakeLists before add_subdirectory so the instrumentation flags reach every
# target. A no-op unless SC_COVERAGE is ON (driven by the *-coverage presets).

# When ON, instrument every target repo-wide so `dev.py coverage` can collect
# counters from the test binaries. The flags are accepted by clang, AppleClang,
# and clang-cl (which reports CXX_COMPILER_ID "Clang"); other compilers are rejected.
option(SC_COVERAGE "Enable LLVM source-based coverage instrumentation" OFF)

if(SC_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SC_COVERAGE requires a Clang/LLVM toolchain (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()
