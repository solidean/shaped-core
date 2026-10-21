// libc++ 22's <float.h> closes its include guard on a partial include, and the macOS 27 SDK's <math.h> makes one.
// Builds with `-std=c++20` or later against MacOSX27.sdk: FLT_MIN is undeclared on Homebrew LLVM 22, fine on LLVM 23 and AppleClang.
//
// <math.h> first is load-bearing: it is what includes <float.h> with __need_infinity_nan defined, for INFINITY and NAN alone.
// Its path only runs under __has_feature(modules), which clang reports true from C++20 on, so -std=c++17 does not reproduce.
// The second include is the ordinary one a user writes (imgui.h does), and it finds the guard already closed.
#include <math.h>
#include <float.h>

float smallest = FLT_MIN;

int main() { return smallest > 0 ? 0 : 1; }
