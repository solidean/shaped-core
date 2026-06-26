#pragma once

// =========================================================================================================
// Compiler detection
// =========================================================================================================
// Conditionally defined: CC_COMPILER_MSVC, CC_COMPILER_CLANG, CC_COMPILER_GCC, CC_COMPILER_MINGW, CC_COMPILER_POSIX

#if defined(_MSC_VER)
#define CC_COMPILER_MSVC
#elif defined(__clang__)
#define CC_COMPILER_CLANG
#elif defined(__GNUC__)
#define CC_COMPILER_GCC
#elif defined(__MINGW32__) || defined(__MINGW64__)
#define CC_COMPILER_MINGW
#else
#error "Unknown compiler"
#endif

#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC) || defined(CC_COMPILER_MINGW)
#define CC_COMPILER_POSIX
#endif

// =========================================================================================================
// Compilation modes
// =========================================================================================================
// Conditionally defined: CC_HAS_RTTI, CC_HAS_CPP_EXCEPTIONS, CC_ASSERT_ENABLED
// From CMake: CC_DEBUG, CC_RELEASE, CC_RELWITHDEBINFO

#ifdef CC_COMPILER_MSVC
#ifdef _CPPRTTI
#define CC_HAS_RTTI
#endif
#ifdef _CPPUNWIND
#define CC_HAS_CPP_EXCEPTIONS
#endif
#elif defined(CC_COMPILER_CLANG)
#if __has_feature(cxx_rtti)
#define CC_HAS_RTTI
#endif
#if __EXCEPTIONS && __has_feature(cxx_exceptions)
#define CC_HAS_CPP_EXCEPTIONS
#endif
#elif defined(CC_COMPILER_GCC)
#ifdef __GXX_RTTI
#define CC_HAS_RTTI
#endif
#if __EXCEPTIONS
#define CC_HAS_CPP_EXCEPTIONS
#endif
#endif

// CC_ASSERT_ENABLED - Assertions are active (0 or 1)
// Assertions are enabled in debug and release-with-debug-info builds by default
// Can be explicitly enabled in release builds with CC_ENABLE_ASSERT_IN_RELEASE
#if defined(CC_DEBUG) || defined(CC_RELWITHDEBINFO) || defined(CC_ENABLE_ASSERT_IN_RELEASE)
#define CC_ASSERT_ENABLED 1
#else
#define CC_ASSERT_ENABLED 0
#endif

// =========================================================================================================
// Operating system detection
// =========================================================================================================
// Conditionally defined: CC_OS_WINDOWS, CC_OS_LINUX, CC_OS_APPLE, CC_OS_BSD, CC_OS_EMSCRIPTEN, CC_OS_WASI
// CC_OS_WASM is an umbrella set for any WebAssembly "OS" (Emscripten or WASI); branch on it for behavior
// shared across the wasm family, and on the specific macro where they differ.
//
// The wasm branches come first: Emscripten reports neither _WIN32 nor __linux__, but it (and some wasi
// toolchains) can leak unix-ish predefines, so detecting __EMSCRIPTEN__/__wasi__ up front avoids misclassification.

#if defined(__EMSCRIPTEN__)
#define CC_OS_EMSCRIPTEN
#define CC_OS_WASM
#elif defined(__wasi__) || defined(__wasm__)
#define CC_OS_WASI
#define CC_OS_WASM
#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32)
#define CC_OS_WINDOWS
#elif defined(__APPLE__) || defined(__MACH__) || defined(macintosh)
#define CC_OS_APPLE
#elif defined(__linux__) || defined(linux)
#define CC_OS_LINUX
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define CC_OS_BSD
#else
#error "Unknown platform"
#endif

// =========================================================================================================
// Target platform detection
// =========================================================================================================
// Conditionally defined: CC_TARGET_PC, CC_TARGET_XBOX, CC_TARGET_MACOS, CC_TARGET_IOS, CC_TARGET_TVOS,
//                        CC_TARGET_ANDROID, CC_TARGET_ORBIS, CC_TARGET_NX, CC_TARGET_WEB, CC_TARGET_MOBILE,
//                        CC_TARGET_CONSOLE

#if defined(CC_OS_WASM)
#define CC_TARGET_WEB
#endif

#if defined(CC_OS_WINDOWS)
#if defined(_DURANGO)
#define CC_TARGET_XBOX
#else
#define CC_TARGET_PC
#endif
#endif

#if defined(CC_OS_APPLE)
#include "TargetConditionals.h"
#if TARGET_OS_MAC
#define CC_TARGET_MACOS
#elif TARGET_OS_IOS
#define CC_TARGET_IOS
#elif TARGET_OS_TV
#define CC_TARGET_TVOS
#else
#error "Unknown Apple platform"
#endif
#endif

#if defined(__ANDROID__)
#define CC_TARGET_ANDROID
#endif

#if defined(__ORBIS__)
#define CC_TARGET_ORBIS
#endif

#if defined(__NX__)
#define CC_TARGET_NX
#endif

#if defined(CC_TARGET_IOS) || defined(CC_TARGET_TVOS) || defined(CC_TARGET_ANDROID)
#define CC_TARGET_MOBILE
#endif

#if defined(CC_TARGET_ORBIS) || defined(CC_TARGET_NX) || defined(CC_TARGET_XBOX)
#define CC_TARGET_CONSOLE
#endif

// =========================================================================================================
// Threading availability
// =========================================================================================================
// CC_HAS_THREADS - whether real OS threads are available (0 or 1).
// Native platforms always have them. On WebAssembly threads are opt-in: Emscripten only enables pthreads
// (and SharedArrayBuffer-backed std::thread) when built with -pthread, which predefines
// __EMSCRIPTEN_PTHREADS__. Single-threaded wasm still compiles <mutex>/<thread>/thread_local fine — they
// degrade to no-ops — so this flag gates behavior, not compilation.

#if defined(CC_OS_WASM)
#if defined(__EMSCRIPTEN_PTHREADS__)
#define CC_HAS_THREADS 1
#else
#define CC_HAS_THREADS 0
#endif
#else
#define CC_HAS_THREADS 1
#endif

// =========================================================================================================
// Public macros
// =========================================================================================================

// CC_PRETTY_FUNC - Current function name as a string (prettier than __func__)
#define CC_PRETTY_FUNC CC_IMPL_PRETTY_FUNC

// CC_FORCE_INLINE - Force function to be inlined
#define CC_FORCE_INLINE CC_IMPL_FORCE_INLINE

// CC_DONT_INLINE - Prevent function from being inlined
#define CC_DONT_INLINE CC_IMPL_DONT_INLINE

// CC_FORCE_INLINE_DEBUGGABLE - Inline in release, normal inline in debug (for debuggability)
#define CC_FORCE_INLINE_DEBUGGABLE CC_IMPL_FORCE_INLINE_DEBUGGABLE

// CC_COLD_FUNC - Mark function as rarely executed (error paths, assertions)
// Usage: CC_COLD_FUNC void handle_error() { ... }
#define CC_COLD_FUNC CC_IMPL_COLD_FUNC

// CC_HOT_FUNC - Mark function as frequently executed (hot path optimization)
// Usage: CC_HOT_FUNC void process_frame() { ... }
#define CC_HOT_FUNC CC_IMPL_HOT_FUNC

// CC_PURE - Function has no side effects and its result depends only on its arguments and the memory it
// reads. Lets the compiler elide redundant calls (only across code that cannot have changed the read memory)
// and pipeline calls across a loop, so an out-of-line wrapper over a pure callee is not pessimized relative
// to the callee. Use ONLY for genuinely pure functions: no writes, no hidden/global state, deterministic —
// e.g. a hash of its byte input. Misuse (marking a function with side effects pure) causes miscompiles.
// Usage: [[nodiscard]] CC_PURE u64 hash_of(span<byte const> data);
#define CC_PURE CC_IMPL_PURE

// CC_BUILTIN_UNREACHABLE - Mark code path as unreachable (UB if reached)
// Usage: default: CC_BUILTIN_UNREACHABLE;
#define CC_BUILTIN_UNREACHABLE CC_IMPL_BUILTIN_UNREACHABLE

// CC_ARRAY_COUNT_OF(arr) - Get compile-time array element count
// Usage: int arr[10]; size_t count = CC_ARRAY_COUNT_OF(arr); // 10
#define CC_ARRAY_COUNT_OF(arr) CC_IMPL_ARRAY_COUNT_OF(arr)

// CC_ASSUME(x) - Hint to compiler that condition x is true (UB if false)
// Usage: CC_ASSUME(ptr != nullptr);
#define CC_ASSUME(x) CC_IMPL_ASSUME(x)

// CC_MACRO_JOIN(a, b) - Concatenate two tokens at preprocessing time
// Usage: CC_MACRO_JOIN(foo_, bar) -> foo_bar
// Note: Indirection ensures arguments are expanded before concatenation
//       Direct ## would concatenate before expansion: MY_PREFIX ## _suffix with MY_PREFIX=foo gives "MY_PREFIX_suffix", not "foo_suffix"
#define CC_MACRO_JOIN(arg1, arg2) CC_IMPL_MACRO_JOIN(arg1, arg2)

// CC_STRINGIFY_EXPR(expr) - Convert expression to string literal, expanding macros first
// Usage: CC_STRINGIFY_EXPR(MY_VALUE) where MY_VALUE=42 -> "42"
// Note: Indirection ensures arguments are expanded before stringification
//       Direct #expr would not expand: #MY_VALUE with MY_VALUE=42 gives "MY_VALUE", not "42"
#define CC_STRINGIFY_EXPR(expr) CC_IMPL_STRINGIFY_EXPR(expr)

// CC_UNUSED(expr) - Suppress unused variable/expression warnings (forces semicolon)
// Usage: CC_UNUSED(result);
// Note: Expression is NOT evaluated, only its type is checked (sizeof is unevaluated context)
#define CC_UNUSED(expr) (void)(sizeof((expr)))

// CC_FORCE_SEMICOLON - Force a semicolon after macro invocation
// Usage: #define MY_MACRO() do_something(); CC_FORCE_SEMICOLON
#define CC_FORCE_SEMICOLON static_assert(true)


// =========================================================================================================
// Implementation details
// =========================================================================================================

#ifdef CC_DEBUG
#define CC_IMPL_FORCE_INLINE_DEBUGGABLE inline
#else
#define CC_IMPL_FORCE_INLINE_DEBUGGABLE CC_IMPL_FORCE_INLINE
#endif

#if defined(CC_COMPILER_MSVC)

#define CC_IMPL_PRETTY_FUNC __FUNCTION__

#define CC_IMPL_FORCE_INLINE __forceinline
#define CC_IMPL_DONT_INLINE __declspec(noinline)

#define CC_IMPL_COLD_FUNC
#define CC_IMPL_HOT_FUNC

#define CC_IMPL_BUILTIN_UNREACHABLE __assume(0)
#define CC_IMPL_ARRAY_COUNT_OF(arr) __crt_countof(arr)
#define CC_IMPL_ASSUME(x) __assume(x)

#elif defined(CC_COMPILER_POSIX)

#define CC_IMPL_PRETTY_FUNC __PRETTY_FUNCTION__

// additional 'inline' is required on gcc and makes no difference on clang
#define CC_IMPL_FORCE_INLINE __attribute__((always_inline)) inline
#define CC_IMPL_DONT_INLINE __attribute__((noinline))

#define CC_IMPL_COLD_FUNC __attribute__((cold))
#define CC_IMPL_HOT_FUNC __attribute__((hot))

#define CC_IMPL_BUILTIN_UNREACHABLE __builtin_unreachable()
#define CC_IMPL_ARRAY_COUNT_OF(arr) (sizeof(arr) / sizeof(arr[0]))
#if defined(CC_COMPILER_CLANG)
#define CC_IMPL_ASSUME(x) __builtin_assume(x)
#else
#define CC_IMPL_ASSUME(x) ((!x) ? __builtin_unreachable() : void(0))
#endif

#else
#error "Unknown compiler"
#endif

// CC_IMPL_PURE keys on the actual compiler builtins, not CC_COMPILER_MSVC: clang-cl defines _MSC_VER (so it
// buckets as MSVC above) yet fully supports GNU attributes, and we want the attribute there. Empty only on
// genuine MSVC, which has no equivalent.
#if defined(__GNUC__) || defined(__clang__)
#define CC_IMPL_PURE [[gnu::pure]] // C++ spelling: unambiguous in a leading attribute-seq (vs __attribute__)
#else
#define CC_IMPL_PURE
#endif

#define CC_IMPL_MACRO_JOIN(arg1, arg2) arg1##arg2
#define CC_IMPL_STRINGIFY_EXPR(expr) #expr
