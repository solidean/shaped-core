/* Force-included by tools/cmake/InfinityNan.cmake, and only where its configure check found INFINITY missing. */
/* The macOS 27 SDK's math.h leaves INFINITY and NAN to <float.h>, as C23 has it. */
/* Clang 22's float.h defines them only for C23 or without __STRICT_ANSI__, so strict -std=c++23 gets neither. */
/* Vendored code that names them then fails: ImPlot, metal-cpp. */
#pragma once

#include <float.h>
#include <math.h>

#ifndef INFINITY
#define INFINITY (__builtin_inff())
#endif

#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif
