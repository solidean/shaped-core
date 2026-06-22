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
// Conditionally defined: CC_OS_WINDOWS, CC_OS_LINUX, CC_OS_APPLE, CC_OS_BSD

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
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
//                        CC_TARGET_ANDROID, CC_TARGET_ORBIS, CC_TARGET_NX, CC_TARGET_MOBILE, CC_TARGET_CONSOLE

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

#define CC_IMPL_MACRO_JOIN(arg1, arg2) arg1##arg2
#define CC_IMPL_STRINGIFY_EXPR(expr) #expr
