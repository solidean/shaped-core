cmake_minimum_required(VERSION 3.28)
project(metal-cpp LANGUAGES CXX)

# Vendored metal-cpp (see vendor-metal-cpp.py for the pinned tag).
# Apple's header-only C++ interface to Metal, Foundation and QuartzCore — how shaped-graphics-metal speaks to Metal
# without a `.mm` source anywhere in the tree.
#
# A static target rather than an INTERFACE one because the headers need exactly one TU to define their symbols;
# src/metal_cpp_impl.cc is ours and provides it, so a consumer links this and defines nothing.
set(METAL_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(metal-cpp STATIC ${METAL_CPP_DIR}/src/metal_cpp_impl.cc)

# SYSTEM so the upstream headers do not trip our warnings in the consuming TU.
# include/ is the search root rather than each framework directory, because metal-cpp's headers reach each other by
# relative path ("../Foundation/Foundation.hpp") and a consumer writes <Metal/Metal.hpp>.
target_include_directories(metal-cpp SYSTEM PUBLIC ${METAL_CPP_DIR}/include)

# The frameworks the wrapper dispatches into. Foundation and QuartzCore are as load-bearing as Metal itself:
# metal-cpp's NS:: types are Foundation objects, and CA::MetalLayer is what a swapchain presents through.
target_link_libraries(metal-cpp PUBLIC
    "-framework Metal"
    "-framework Foundation"
    "-framework QuartzCore"
)

target_compile_features(metal-cpp PUBLIC cxx_std_17)

# PUBLIC rather than PRIVATE: the macro decides the visibility attribute on definitions that live in the headers, so a
# consumer compiling them without it would disagree with this TU about the same symbol.
target_compile_definitions(metal-cpp PUBLIC METALCPP_SYMBOL_VISIBILITY_HIDDEN)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
target_compile_options(metal-cpp PRIVATE -w -fvisibility=hidden)

set_property(TARGET metal-cpp PROPERTY POSITION_INDEPENDENT_CODE ON)
