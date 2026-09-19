#include "stacktrace.hh"

#if CC_HAS_STACKTRACE

#include <clean-core/platform/symbolize.hh> // cc::impl::with_dbghelp: rendering a trace symbolizes on Windows
#include <clean-core/string/string.hh>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h> // emscripten_get_callstack, the only route to frame text here
#else
#include <string> // std::to_string(std::stacktrace) is the only renderer the standard offers
#endif

#if defined(__EMSCRIPTEN__)

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/platform/impl/wasm_frames.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace
{
/// The deepest trace this renders.
/// Matches what a capture is willing to produce, so nothing is lost between the two.
constexpr isize max_trace_frames = 128;

/// One frame, as a line a person reads.
///
/// `module+offset` is the fallback rather than an error: a build with no name section still names WHERE a frame is,
/// which is exactly what the offline resolver needs and all it needs.
[[nodiscard]] cc::string render(cc::symbol_info const& info, u32 address)
{
    if ((address & cc::impl::wasm_js_frame_bit) != 0)
    {
        auto const line = address & ~cc::impl::wasm_js_frame_bit;
        return info.has_function() ? cc::format("{} (js:{})", info.function, line) : cc::format("js:{}", line);
    }

    if (info.has_function())
        return cc::format("{} (wasm+{:#x})", info.function, address);
    return cc::format("wasm+{:#x}", address);
}

/// The deepest stack any of the overloads below reads.
/// A fixed buffer, so a capture allocates nothing beyond the trace it returns.
constexpr isize capture_buffer_frames = max_trace_frames;
} // namespace

cc::stacktrace cc::stacktrace::from_frames(cc::span<void* const> frames, std::size_t max_depth) noexcept
{
    auto result = cc::stacktrace();

    auto symbols = cc::symbolizer();
    for (auto const* f = frames.begin(); f != frames.end(); ++f)
    {
        if (std::size_t(result._frames.size()) >= max_depth)
            break;

        auto const address = u32(reinterpret_cast<uintptr_t>(*f));
        result._frames.push_back(cc::stacktrace_entry(address, render(symbols.resolve(*f), address)));
    }

    return result;
}

// **The skew is cc::capture_stack's problem, not this one's.**
//
// The previous version of this looked for its own frames by NAME, which found nothing in a build with no name
// section and silently kept them -- the same class of bug as trusting Emscripten's own frame offset.
// Going through the capture means one place gets it right and a test pins it.
//
// The `+ 1` in each is this overload's own frame, which a caller never means to see.

cc::stacktrace cc::stacktrace::current() noexcept
{
    void* frames[capture_buffer_frames] = {};
    auto const captured = cc::capture_stack(cc::span<void*>(frames), 1);
    return from_frames(cc::span<void* const>(frames, captured.count), ~std::size_t(0));
}

cc::stacktrace cc::stacktrace::current(std::size_t skip) noexcept
{
    void* frames[capture_buffer_frames] = {};
    auto const captured = cc::capture_stack(cc::span<void*>(frames), isize(skip) + 1);
    return from_frames(cc::span<void* const>(frames, captured.count), ~std::size_t(0));
}

cc::stacktrace cc::stacktrace::current(std::size_t skip, std::size_t max_depth) noexcept
{
    void* frames[capture_buffer_frames] = {};
    auto const captured = cc::capture_stack(cc::span<void*>(frames), isize(skip) + 1);
    return from_frames(cc::span<void* const>(frames, captured.count), max_depth);
}

#endif // __EMSCRIPTEN__

cc::string cc::to_string(cc::stacktrace const& trace)
{
#if defined(__EMSCRIPTEN__)
    auto out = cc::string();
    for (auto const& frame : trace)
    {
        if (!out.empty())
            out += '\n';
        out += frame.description();
    }
    return out;
#else
    // Under the DbgHelp lock, because on Windows this symbolizes: std::to_string(std::stacktrace) resolves every
    // frame through the same process-wide DbgHelp state a cc::symbolizer uses, and the STL's own lock does not
    // serialize against ours.
    // Rendering a trace on one thread while another symbolizes is otherwise the documented single-threaded API being
    // used from two threads, which loses names before it corrupts anything.
    // A pass-through everywhere else, where the renderer touches no such global.
    auto out = cc::string();
    cc::impl::with_dbghelp([&] { out = cc::string(std::to_string(trace)); });
    return out;
#endif
}

#endif // CC_HAS_STACKTRACE
