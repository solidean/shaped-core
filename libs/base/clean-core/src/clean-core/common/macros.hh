#pragma once

// =========================================================================================================
// Platform detection — the ONLY place raw toolchain predefines may be read
// =========================================================================================================
// This header translates the compiler's raw predefined macros (_MSC_VER, __clang__, _WIN32, __APPLE__,
// _M_X64, __aarch64__, __ANDROID__, …) into a small, orthogonal set of CC_* macros. Downstream code MUST
// branch on these CC_* macros and MUST NOT read the raw predefines directly — that keeps the messy,
// overlapping vendor macros (e.g. clang-cl defines _MSC_VER; Android also defines __linux__; TARGET_OS_MAC
// is set on iOS too) confined to one audited place. The five axes are independent: query each separately.
//
//   Compiler: CC_COMPILER_{CLANG, MSVC, GCC}            — who is compiling (clang-cl counts as CLANG)
//   Arch:     CC_ARCH_{X64, X86, ARM64, ARM32, WASM32}  — target CPU / instruction set
//   OS:       CC_OS_{WINDOWS, LINUX, MACOS, IOS, TVOS, ANDROID, EMSCRIPTEN, WASI}
//   ABI:      CC_ABI_{MSVC, SYSV, ANDROID, DARWIN, WASM} — calling convention / object format family
//   Platform: CC_PLATFORM_{DESKTOP, MOBILE, WEB, CONSOLE} — coarse device class (orthogonal to OS: an Xbox
//             is OS=WINDOWS, Platform=CONSOLE)
//
// Exactly one macro is defined per axis. CC_HAS_64BIT_POINTERS (0 or 1) is derived from Arch — pointer
// width is not an axis of its own, but it is the thing byte-count code actually means.

// --- Compiler ---
// clang is checked first: clang-cl also defines _MSC_VER but is clang, and bucketing it as CLANG (not MSVC)
// lets it take the GNU-attribute paths it supports. MinGW (defines __GNUC__) folds into GCC.
#if defined(__clang__)
#define CC_COMPILER_CLANG
#elif defined(_MSC_VER)
#define CC_COMPILER_MSVC
#elif defined(__GNUC__)
#define CC_COMPILER_GCC
#else
#error "Unknown compiler"
#endif

// --- Architecture ---
#if defined(__wasm32__) || (defined(__wasm__) && !defined(__wasm64__))
#define CC_ARCH_WASM32
#elif defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
#define CC_ARCH_X64
#elif defined(_M_ARM64) || defined(__aarch64__)
#define CC_ARCH_ARM64
#elif defined(_M_IX86) || defined(__i386__)
#define CC_ARCH_X86
#elif defined(_M_ARM) || defined(__arm__)
#define CC_ARCH_ARM32
#else
#error "Unknown architecture"
#endif

// --- Pointer width (derived from Arch; always defined, 0 or 1) ---
// Register width and pointer width are separate questions here, and only one of them is settled:
//
//   * 64-bit REGISTERS are required. Everything we ship assumes 64-bit arithmetic is a single operation
//     (cc::atomic<u64>, the fused refcount, the tagged control words).
//   * Pointer width is NOT 64 everywhere. wasm32 is the case that matters: 64-bit registers, 32-bit
//     pointers. So a pointer is 4 B there, and every struct footprint derived from one shrinks with it —
//     cc::small_vector's 48 B is a 64-bit-pointer statement, not a universal one.
//
// Branch on this — never on the arch, and never on a hand-rolled sizeof(void*) == 8 at the use site.
#if defined(CC_ARCH_WASM32) || defined(CC_ARCH_X86) || defined(CC_ARCH_ARM32)
#define CC_HAS_64BIT_POINTERS 0
#else
#define CC_HAS_64BIT_POINTERS 1
#endif

// =========================================================================================================
// Compilation modes
// =========================================================================================================
// Conditionally defined: CC_HAS_RTTI, CC_HAS_CPP_EXCEPTIONS
// Always defined, 0 or 1: CC_ASSERT_ENABLED, CC_HAS_THREADS
// From CMake: CC_DEBUG, CC_RELEASE, CC_RELWITHDEBINFO, CC_SINGLE_THREADED
//
// CMake only ever defines inputs; this header owns every derivation and defines the outputs
// unconditionally. So CC_ENABLE_ASSERT_IN_RELEASE feeds CC_ASSERT_ENABLED, and CC_SINGLE_THREADED feeds
// CC_HAS_THREADS — none of the derived macros is itself overridable.

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

// --- Operating system ---
// Order matters: Emscripten/WASI leak unix-ish predefines, and Android also defines __linux__, so the more
// specific OSes are matched before the generic ones. The Apple split keys off <TargetConditionals.h> and
// checks iOS/tvOS before macOS because TARGET_OS_MAC is set on all Apple platforms.
#if defined(__EMSCRIPTEN__)
#define CC_OS_EMSCRIPTEN
#elif defined(__wasi__)
#define CC_OS_WASI
#elif defined(_WIN32)
#define CC_OS_WINDOWS
#elif defined(__ANDROID__)
#define CC_OS_ANDROID
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#define CC_OS_IOS
#elif TARGET_OS_TV
#define CC_OS_TVOS
#elif TARGET_OS_OSX
#define CC_OS_MACOS
#else
#error "Unknown Apple platform"
#endif
#elif defined(__linux__)
#define CC_OS_LINUX
#else
#error "Unknown operating system"
#endif

// --- ABI (calling convention / object format family) ---
#if defined(CC_OS_WINDOWS)
#define CC_ABI_MSVC
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
#define CC_ABI_DARWIN
#elif defined(CC_OS_ANDROID)
#define CC_ABI_ANDROID
#elif defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_WASI)
#define CC_ABI_WASM
#else
#define CC_ABI_SYSV
#endif

// --- Platform (coarse device class; orthogonal to OS) ---
#if defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_WASI)
#define CC_PLATFORM_WEB
#elif defined(CC_OS_IOS) || defined(CC_OS_TVOS) || defined(CC_OS_ANDROID)
#define CC_PLATFORM_MOBILE
#elif defined(_DURANGO) || defined(__ORBIS__) || defined(__NX__)
#define CC_PLATFORM_CONSOLE
#else
#define CC_PLATFORM_DESKTOP
#endif

// =========================================================================================================
// Threading availability
// =========================================================================================================
// CC_HAS_THREADS - whether real OS threads are available (0 or 1).
// Native platforms always have them. On WebAssembly threads are opt-in: Emscripten only enables pthreads
// (and SharedArrayBuffer-backed std::thread) when built with -pthread, which predefines
// __EMSCRIPTEN_PTHREADS__. Single-threaded wasm still compiles <mutex>/<thread>/thread_local fine — they
// degrade to no-ops — so this flag gates behavior, not compilation.
//
// CC_SINGLE_THREADED (from CMake: SC_THREADS=OFF) forces 0 on a platform that HAS threads, which is what
// makes the single-threaded mode developable natively instead of only under wasm. It forces threads OFF
// only: wasm without -pthread genuinely has none, so there is deliberately no way to force them ON.
// Absent the define the autodetect below stands, which is also the path when clean-core is consumed via
// add_subdirectory without our root CMakeLists.

#if defined(CC_SINGLE_THREADED)
#define CC_HAS_THREADS 0
#elif defined(CC_PLATFORM_WEB)
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

#elif defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)

#define CC_IMPL_PRETTY_FUNC __PRETTY_FUNCTION__

// additional 'inline' is required on gcc and makes no difference on clang
#define CC_IMPL_FORCE_INLINE __attribute__((always_inline)) inline
#define CC_IMPL_DONT_INLINE __attribute__((noinline))

// C++ spelling ([[gnu::cold]], not __attribute__((cold))): a leading __attribute__ followed by a C++
// attribute-seq (e.g. CC_COLD_FUNC [[nodiscard]]) is rejected by some clangs (older Apple clang).
#define CC_IMPL_COLD_FUNC [[gnu::cold]]
#define CC_IMPL_HOT_FUNC [[gnu::hot]]

#define CC_IMPL_BUILTIN_UNREACHABLE __builtin_unreachable()
#define CC_IMPL_ARRAY_COUNT_OF(arr) (sizeof(arr) / sizeof(arr[0]))
#if defined(CC_COMPILER_CLANG)
#define CC_IMPL_ASSUME(x) __builtin_assume(x)
#else
#define CC_IMPL_ASSUME(x) ((!(x)) ? __builtin_unreachable() : void(0))
#endif

#else
#error "Unknown compiler"
#endif

// CC_PURE: GNU pure attribute on clang/gcc, empty on MSVC (no equivalent).
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
#define CC_IMPL_PURE [[gnu::pure]] // C++ spelling: unambiguous in a leading attribute-seq (vs __attribute__)
#else
#define CC_IMPL_PURE
#endif

#define CC_IMPL_MACRO_JOIN(arg1, arg2) arg1##arg2
#define CC_IMPL_STRINGIFY_EXPR(expr) #expr
