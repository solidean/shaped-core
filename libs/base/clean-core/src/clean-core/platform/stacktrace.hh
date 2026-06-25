#pragma once

#include <clean-core/common/macros.hh>

// cc::stacktrace mirrors std::stacktrace where the C++23 <stacktrace> header is available, and degrades to
// an empty stub where it is not. Emscripten / WASI libc++ currently ship no <stacktrace>, yet code that
// captures a trace (the default assert handler, cc::any_error payloads) must still compile and link there.
//
// CC_HAS_STACKTRACE reflects which path is active. The stub is a complete, allocatable value type that
// reports an empty trace, so storing or passing a cc::stacktrace needs no #ifdef. Code that *renders* a
// trace (calls description()/to_string) is the only thing that must guard on CC_HAS_STACKTRACE, because
// only the real std::stacktrace can produce frame text.

#if defined(__has_include)
#if __has_include(<stacktrace>)
#define CC_HAS_STACKTRACE 1
#else
#define CC_HAS_STACKTRACE 0
#endif
#else
#define CC_HAS_STACKTRACE 0
#endif

#if CC_HAS_STACKTRACE

#include <stacktrace>

namespace cc
{
/// Type alias for std::stacktrace
/// Represents a snapshot of the program's call stack
/// Makes the clean-core stdlib more consistent while delegating to standard library
/// Usage:
///   cc::stacktrace trace = cc::stacktrace::current();
///   for (auto const& entry : trace) {
///       std::cout << entry.description() << '\n';
///   }
using stacktrace = std::stacktrace;

/// Type alias for std::stacktrace_entry
/// Represents a single frame in a stacktrace
using stacktrace_entry = std::stacktrace_entry;
} // namespace cc

#else // CC_HAS_STACKTRACE

#include <cstddef>

namespace cc
{
/// Stub stacktrace frame for toolchains without <stacktrace> (Emscripten / WASI).
/// Carries no information; present only so the empty stacktrace stub can be iterated.
struct stacktrace_entry
{
    [[nodiscard]] constexpr bool operator==(stacktrace_entry const&) const = default;
};

/// Stub stacktrace for toolchains without <stacktrace> (see CC_HAS_STACKTRACE).
/// Always empty: current() yields a trace of size 0. Stays API-shaped enough that storing/iterating a
/// cc::stacktrace compiles unchanged; it simply has nothing to report.
struct stacktrace
{
    /// Capture the current call stack. On the stub this is always an empty trace.
    /// The skip / max-depth parameters mirror std::stacktrace::current and are ignored.
    [[nodiscard]] static stacktrace current() noexcept { return {}; }
    [[nodiscard]] static stacktrace current(std::size_t) noexcept { return {}; }
    [[nodiscard]] static stacktrace current(std::size_t, std::size_t) noexcept { return {}; }

    [[nodiscard]] bool empty() const noexcept { return true; }
    [[nodiscard]] std::size_t size() const noexcept { return 0; }

    [[nodiscard]] stacktrace_entry const* begin() const noexcept { return nullptr; }
    [[nodiscard]] stacktrace_entry const* end() const noexcept { return nullptr; }

    [[nodiscard]] constexpr bool operator==(stacktrace const&) const = default;
};
} // namespace cc

#endif // CC_HAS_STACKTRACE
