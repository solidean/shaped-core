# Floating-point contraction is off for the whole build, like fast-math: shaped-core does not support it.
# Included once from the root CMakeLists before add_subdirectory(extern), so vendored code is built the same way.
#
# Contraction lets the compiler fuse `a * b + c` into one FMA wherever the target has one, which moves the result's last bit.
# Left at their defaults, clang contracts within an expression, GCC across statements, and MSVC not at all.
# So one source gave different bits per toolchain, per optimization level and per SC_X64_LEVEL.
# Fusion is spelled where it is wanted: cimd's mul_add, or std::fma.
#
# MSVC needs no flag: /fp:precise, its default, does not contract unless /fp:contract is given.

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
    add_compile_options("/clang:-ffp-contract=off")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-ffp-contract=off)
endif()
