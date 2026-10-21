# INFINITY and NAN where the toolchain's headers fail to provide them under strict C++23.
#
# Homebrew clang 22 with the macOS 27 SDK is the case: the SDK's math.h defers both to <float.h>, which defines them only for C23 or gnu++.
# A file that includes <float.h> before <math.h> then sees neither; Apple clang 21 with the same SDK is fine, and so are Linux and Windows.
# So the check decides, not the platform: the shim goes in only where the snippet fails, and drops out once the toolchains agree again.
#
# Force-included rather than patched where it is used, because the users are vendored headers (ImPlot, metal-cpp) that a re-vendor overwrites.
# Included once from the root CMakeLists after BuildConfig, so consumers get it through shaped-core::build-config as well.

if(MSVC)
    return()
endif()

include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles([[
#include <float.h>
#include <math.h>
float infinity = INFINITY;
float nan_value = NAN;
int main() { return infinity > 0.0f && nan_value != nan_value ? 0 : 1; }
]] SC_HAS_INFINITY_AND_NAN)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT SC_HAS_INFINITY_AND_NAN)
    message(STATUS "INFINITY / NAN missing under strict C++23 with this toolchain: force-including tools/cmake/infinity-nan-shim.h")
    # C and the Objective-C dialects too, since the metal backend's .mm files reach metal-cpp; never assembler.
    # Joined (-include<path>) so it stays one argument, which option de-duplication then sees as one flag.
    sc_build_options(COMPILE "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-include${CMAKE_CURRENT_LIST_DIR}/infinity-nan-shim.h>")
endif()
