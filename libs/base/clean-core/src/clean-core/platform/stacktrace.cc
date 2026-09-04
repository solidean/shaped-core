#include "stacktrace.hh"

#if CC_HAS_STACKTRACE

#include <clean-core/string/string.hh>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h> // emscripten_get_callstack, the only route to frame text here
#else
#include <string> // std::to_string(std::stacktrace) is the only renderer the standard offers
#endif

#if defined(__EMSCRIPTEN__)

using namespace cc::primitive_defines;

namespace
{
/// What emscripten_get_callstack is asked for.
/// The C stack is the one worth reporting: the JS frames above it are the module loader and the host, and they push
/// everything interesting past any sensible depth cap.
/// Demangling is on because the alternative is reading mangled C++ by hand.
constexpr int callstack_flags = EM_LOG_C_STACK | EM_LOG_DEMANGLE;

/// The whole callstack as one string, or empty when the platform reports none.
/// Its length is asked for first, since it is not knowable in advance and a short buffer truncates silently.
cc::string capture_callstack_text()
{
    auto const needed = ::emscripten_get_callstack(callstack_flags, nullptr, 0);
    if (needed <= 0)
        return {};

    auto text = cc::string::create_filled(needed, '\0');
    auto const written = ::emscripten_get_callstack(callstack_flags, text.data(), needed);
    if (written <= 0)
        return {};

    // The reported count includes the terminator, which is not part of the text.
    auto size = isize(written);
    while (size > 0 && text[size - 1] == '\0')
        --size;
    text.resize_down_to(size);
    return text;
}
} // namespace

cc::stacktrace cc::stacktrace::current() noexcept
{
    return current(0, ~std::size_t(0));
}

cc::stacktrace cc::stacktrace::current(std::size_t skip) noexcept
{
    return current(skip, ~std::size_t(0));
}

cc::stacktrace cc::stacktrace::current(std::size_t skip, std::size_t max_depth) noexcept
{
    auto result = cc::stacktrace();
    auto const text = capture_callstack_text();

    // One frame per line, which is the shape emscripten_get_callstack emits.
    // The capture's OWN frames come back too, and a caller means "start at me", exactly where
    // std::stacktrace::current begins -- so everything up to and including the last current() frame goes first, and
    // `skip` counts from there.
    //
    // Found by name rather than by counting, because how many frames the overload chain contributes depends on what
    // the optimizer inlined and that is not a number to hard-code.
    // Where the name section was stripped nothing matches and the capture's own frames stay: that is the Release wasm
    // build, where every frame is an index anyway.
    auto const own = cc::string_view("cc::stacktrace::current");

    auto const each_line = [&](auto&& visit)
    {
        auto begin = isize(0);
        for (auto i = isize(0); i <= text.size(); ++i)
        {
            if (i < text.size() && text[i] != '\n')
                continue;
            auto const line = text.subview({.start = begin, .end = i});
            begin = i + 1;
            if (!line.empty())
                visit(line);
        }
    };

    auto first_caller = isize(0);
    auto index = isize(0);
    each_line(
        [&](cc::string_view line)
        {
            ++index;
            if (line.contains(own))
                first_caller = index;
        });

    auto seen = isize(0);
    auto dropped = std::size_t(0);
    each_line(
        [&](cc::string_view line)
        {
            if (seen++ < first_caller)
                return;
            if (dropped < skip)
            {
                ++dropped;
                return;
            }
            if (std::size_t(result._frames.size()) >= max_depth)
                return;
            result._frames.push_back(cc::stacktrace_entry(cc::string(line)));
        });

    return result;
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
    return cc::string(std::to_string(trace));
#endif
}

#endif // CC_HAS_STACKTRACE
