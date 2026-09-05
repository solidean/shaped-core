#pragma once

#include <clean-core/common/macros.hh>

// cc::stacktrace mirrors std::stacktrace where the C++23 <stacktrace> header is available, and degrades to an empty stub where it is not.
// WASI libc++ currently ships no <stacktrace>, yet code that captures a trace — the default assert handler, cc::any_error payloads — must still compile and link there.
//
// Emscripten ships no <stacktrace> either but is NOT the stub: emscripten_get_callstack() renders the current wasm
// call stack as text, which is the one thing the stub cannot do, so it gets a backend of its own below.
// It reports frame TEXT rather than addresses, because that is what the platform hands back — there is no address a
// cc::symbolizer could resolve afterwards, which is why this is not routed through cc::capture_stack.
// Names in that text come from the wasm name section, which the wasm presets keep with --profiling-funcs; without it
// the frames are still there and read as indices rather than names.
//
// CC_HAS_STACKTRACE reflects which path is active.
// Our CMake defines it from a link probe (clean-core/cmake/DetectStacktraceLib.cmake) and that verdict wins:
// the header alone can only prove <stacktrace> exists, never that anything implements it — a libstdc++ without libstdc++exp has the header and none of the symbols.
// The stub is a complete, allocatable value type that reports an empty trace, so storing or passing a cc::stacktrace needs no #ifdef.
// Only code that *renders* a trace, by calling description() or to_string, must guard on CC_HAS_STACKTRACE — a real std::stacktrace is the only thing that can produce frame text.

#ifndef CC_HAS_STACKTRACE
#if defined(__EMSCRIPTEN__)
#define CC_HAS_STACKTRACE 1
#elif defined(__has_include)
#if __has_include(<stacktrace>)
#define CC_HAS_STACKTRACE 1
#else
#define CC_HAS_STACKTRACE 0
#endif
#else
#define CC_HAS_STACKTRACE 0
#endif
#endif

#if defined(__EMSCRIPTEN__) && CC_HAS_STACKTRACE

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

namespace cc
{
struct stacktrace_entry;
struct stacktrace;

/// Renders a whole trace as text, one frame per line.
/// The renderer rather than std::to_string, so a call site needs no #if to know which backend it got.
[[nodiscard]] cc::string to_string(cc::stacktrace const& trace);
} // namespace cc

/// One frame of an Emscripten trace, which carries its own text because that is all the platform reports.
/// source_file() and source_line() have no counterpart here and are deliberately absent rather than faked.
struct cc::stacktrace_entry
{
    stacktrace_entry() = default;
    explicit stacktrace_entry(cc::string text) : _text(cc::move(text)) {}

    [[nodiscard]] cc::string const& description() const { return _text; }
    [[nodiscard]] bool operator==(stacktrace_entry const& rhs) const { return _text == rhs._text; }

private:
    cc::string _text;
};

/// A snapshot of the wasm call stack, captured through emscripten_get_callstack.
/// Allocates, exactly as std::stacktrace does, so it belongs at analysis time rather than in a crash handler --
/// cc::capture_stack is the allocation-free one, and on wasm it has nothing to symbolize against.
struct cc::stacktrace
{
    [[nodiscard]] static stacktrace current() noexcept;
    [[nodiscard]] static stacktrace current(std::size_t skip) noexcept;
    [[nodiscard]] static stacktrace current(std::size_t skip, std::size_t max_depth) noexcept;

    [[nodiscard]] bool empty() const noexcept { return _frames.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return std::size_t(_frames.size()); }

    [[nodiscard]] stacktrace_entry const* begin() const noexcept { return _frames.begin(); }
    [[nodiscard]] stacktrace_entry const* end() const noexcept { return _frames.end(); }

    /// Frame by frame, because cc::vector carries no container-level operator== of its own.
    /// Pointers rather than an index, so this header needs no primitive-width name of its own.
    [[nodiscard]] bool operator==(stacktrace const& rhs) const
    {
        if (_frames.size() != rhs._frames.size())
            return false;
        auto const* b = rhs._frames.begin();
        for (auto const* a = _frames.begin(); a != _frames.end(); ++a, ++b)
            if (!(*a == *b))
                return false;
        return true;
    }

private:
    cc::vector<stacktrace_entry> _frames;
};

#elif CC_HAS_STACKTRACE

#include <clean-core/string/string.hh>

#include <stacktrace>

namespace cc
{
/// A snapshot of the program's call stack, aliased to std::stacktrace.
using stacktrace = std::stacktrace;

/// One frame of a stacktrace, aliased to std::stacktrace_entry.
using stacktrace_entry = std::stacktrace_entry;

/// Renders a whole trace as text, one frame per line.
/// Exists on every backend that has frame text, so a call site needs no #if to know which one it got.
[[nodiscard]] cc::string to_string(cc::stacktrace const& trace);
} // namespace cc

#else // CC_HAS_STACKTRACE

#include <clean-core/fwd.hh> // std::size_t, which the stub's signatures mirror

namespace cc
{
// Declared before the definitions below, which spell their names qualified.
// This branch's declarations cannot move to fwd.hh: with <stacktrace> both names are aliases of the std types, so there is nothing there to declare.
struct stacktrace_entry;
struct stacktrace;
} // namespace cc

/// Stub stacktrace frame for toolchains without <stacktrace> (Emscripten / WASI).
/// Carries no information; present only so the empty stacktrace stub can be iterated.
struct cc::stacktrace_entry
{
    [[nodiscard]] constexpr bool operator==(stacktrace_entry const&) const = default;
};

/// Stub stacktrace for toolchains without <stacktrace> — see CC_HAS_STACKTRACE.
/// Always empty, so current() yields a trace of size 0.
/// Stays API-shaped enough that storing and iterating a cc::stacktrace compiles unchanged; it simply has nothing to report.
struct cc::stacktrace
{
    /// Capture the current call stack, which on the stub is always an empty trace.
    /// The skip and max-depth parameters mirror std::stacktrace::current and are ignored.
    [[nodiscard]] static stacktrace current() noexcept { return {}; }
    [[nodiscard]] static stacktrace current(std::size_t) noexcept { return {}; }
    [[nodiscard]] static stacktrace current(std::size_t, std::size_t) noexcept { return {}; }

    [[nodiscard]] bool empty() const noexcept { return true; }
    [[nodiscard]] std::size_t size() const noexcept { return 0; }

    [[nodiscard]] stacktrace_entry const* begin() const noexcept { return nullptr; }
    [[nodiscard]] stacktrace_entry const* end() const noexcept { return nullptr; }

    [[nodiscard]] constexpr bool operator==(stacktrace const&) const = default;
};

#endif // CC_HAS_STACKTRACE
