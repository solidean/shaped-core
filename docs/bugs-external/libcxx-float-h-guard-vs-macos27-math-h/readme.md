# libc++ 22's `<float.h>` loses `FLT_MIN` after the macOS 27 SDK's `<math.h>`

**Status:** fixed upstream in LLVM 23 ([llvm-project #175849](https://github.com/llvm/llvm-project/pull/175849) removed libc++'s `<float.h>`), worked around in shaped-core.
**Affects:** Homebrew `llvm@22` (22.1.8) with `-std=c++20` or later against `MacOSX27.sdk`.
Not LLVM 21, not LLVM 23, not AppleClang, not any SDK before 27, not C++17.
**Found by:** shaped-rendering, whose imgui include stopped seeing `FLT_MIN` and `FLT_MAX` after a Mac moved to macOS 27.

## What happens

Three headers each do something reasonable, and the combination drops every `FLT_*` macro from the rest of the translation unit.

1. The macOS 27 SDK's `usr/include/math.h` gets `INFINITY` and `NAN` from `<float.h>` rather than defining them itself.
   It asks for those alone: `#define __need_infinity_nan`, `#include <float.h>`, `#undef __need_infinity_nan`.
   It only takes this path under `__has_feature(modules)`, which clang reports true from C++20 on.
2. libc++'s `<float.h>` is found first, because libc++'s directory precedes clang's resource directory.
   It is a thin wrapper with a plain include guard, `_LIBCPP_FLOAT_H`, around an `#include_next <float.h>`.
   It sets that guard on this partial include as on any other.
3. clang 22's own `<float.h>` honours `__need_infinity_nan`: it includes `__float_infinity_nan.h` and skips `__float_float.h`, which holds `FLT_MIN` and the rest.

So the first `<float.h>` delivers only `INFINITY` and `NAN`, and every later `#include <float.h>` stops at libc++'s closed guard.

```
Homebrew clang 22.1.8   MacOSX26.5.sdk  c++23  ok
Homebrew clang 22.1.8   MacOSX27.0.sdk  c++17  ok
Homebrew clang 22.1.8   MacOSX27.0.sdk  c++23  REPRODUCED  use of undeclared identifier 'FLT_MIN'
Homebrew clang 21.1.8   MacOSX27.0.sdk  c++23  ok
LLVM 23.1.3             MacOSX27.0.sdk  c++23  ok
Apple clang 21.0.0      MacOSX27.0.sdk  c++23  ok
```

Each `ok` row is missing exactly one of the three ingredients.
The 26 SDK's `math.h` defines `NAN` itself and never includes `<float.h>`.
Under C++17 the SDK's modules branch is not taken.
clang 21's `<float.h>` ignores `__need_infinity_nan` and defines everything, so the early guard costs nothing.
LLVM 23's libc++ and Apple's libc++ ship no `<float.h>` at all, so clang's own header answers every include.

## Why it is the toolchain and not us

`repro.cc` is two system includes and one use of `FLT_MIN`, which the C and C++ standards both promise from `<float.h>`.
The failing configuration differs from a passing one by the SDK alone, or by the LLVM release alone.
Upstream reached the same conclusion: #175849 removes the wrapper because it "leads to difficulties with modules, especially when it comes to supporting the various `__need` macros".

## Reproducing

```bash
uv run run.py                                   # Homebrew clangs, AppleClang, every macOS SDK found
uv run run.py --clang <llvm-23>/bin/clang++      # add another clang, e.g. an unpacked LLVM release
```

Exit code 1 means it reproduced, 0 means it did not.
A configuration that reproduces is retried with the in-tree shim on its include path, and a `+ shim` row says whether that still holds.

By hand:

```bash
/opt/homebrew/opt/llvm@22/bin/clang++ -std=c++23 -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX27.sdk -fsyntax-only repro.cc
```

## The workaround in shaped-core

[LibcxxFloatH.cmake](../../../tools/cmake/LibcxxFloatH.cmake) applies to Homebrew clang below 23 on Apple.
It puts [libcxx-float-h/float.h](../../../tools/cmake/libcxx-float-h/float.h) ahead of libc++'s on the C++ include path.
That header reopens `_LIBCPP_FLOAT_H` and forwards to libc++'s, so every include reaches clang's `<float.h>`, which tracks the `__need_*` requests itself.
It goes through `sc_build_options`, so vendored code and projects consuming shaped-core get it too.

Building against an older SDK also avoids it, but only on a machine that has one.
The Command Line Tools 27 still ship `MacOSX26.sdk`, and Xcode 27 does not.
There is no `dev.py doctor` probe: the workaround is automatic wherever our build flags reach, so a warning would only be noise.

## What to check when the toolchain moves

The macOS presets pin the versioned `llvm@22` formula, and Homebrew only adds an `llvm@23` once LLVM 24 is the unversioned `llvm`.
When the presets move to LLVM 23 or later, the module's version gate already turns the shim off.
Run `run.py --clang` against the new clang, and if it reports `ok`, delete the module, the header and this directory in the same change.
