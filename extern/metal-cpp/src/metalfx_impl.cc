// The one translation unit that gives metal-cpp's MetalFX selector and class symbols a home.
//
// The same arrangement as metal_cpp_impl.cc, for the fourth framework: MetalFX is its own target, so only what links
// metal-cpp-metalfx loads the framework, and MTLFX_PRIVATE_IMPLEMENTATION is set here alone.

#define MTLFX_PRIVATE_IMPLEMENTATION

#include <MetalFX/MetalFX.hpp>
