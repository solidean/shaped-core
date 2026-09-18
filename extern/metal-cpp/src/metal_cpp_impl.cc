// The one translation unit that gives metal-cpp's selector and class symbols a home.
//
// metal-cpp is header-only, but its headers declare the Objective-C selector and class handles `extern` by default and
// define them only where these three macros are set — so exactly one TU in the link must do this, and this is it.
// Authored here rather than copied from upstream, the way extern/stb/src/stb.c is.
//
// QuartzCore comes along because the metal backend's swapchain draws into a CAMetalLayer, and CA_PRIVATE_IMPLEMENTATION
// is a third symbol home rather than something MTL_PRIVATE_IMPLEMENTATION covers.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
