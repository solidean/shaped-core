// Put ahead of libc++'s own <float.h> by tools/cmake/LibcxxFloatH.cmake, for Homebrew LLVM below 23 on Apple only.
// The macOS 27 SDK's <math.h> includes <float.h> with __need_infinity_nan defined, to get INFINITY and NAN alone.
// libc++ 22's <float.h> forwards that partial request to clang's <float.h> but closes its own include guard anyway,
// so every later <float.h> in the TU is a no-op and FLT_MIN, FLT_MAX and the rest are never defined.
// Reopening the guard lets each inclusion reach clang's <float.h>, which tracks the __need_* requests itself and is idempotent.
// LLVM 23 removed libc++'s <float.h> (llvm-project #175849), and with it this header: delete both when the presets move to it.
#undef _LIBCPP_FLOAT_H
#include_next <float.h>
