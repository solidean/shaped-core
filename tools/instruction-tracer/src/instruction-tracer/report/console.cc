#include "console.hh"

#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/format.hh>
#include <io.h>
#include <stdio.h>

#include <cstdlib>

namespace itrace
{
namespace
{
constexpr cc::string_view reset = "\033[0m";

bool g_enabled = false;

bool is_terminal(FILE* stream)
{
    return _isatty(_fileno(stream)) != 0;
}

/// Old conhost needs to be told it speaks ANSI; Windows Terminal already does. Failure is fine —
/// it only means we are not on a VT-capable console, and the caller's isatty check already gates us.
void enable_windows_vt()
{
    auto* const handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return;

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode))
        return;

    SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

cc::string wrap(cc::string_view code, cc::string_view s)
{
    if (!g_enabled)
        return cc::string(s);

    return cc::format("{}{}{}", code, s, reset);
}
} // namespace

void configure_console(color_mode mode)
{
    switch (mode)
    {
    case color_mode::always:
        g_enabled = true;
        break;

    case color_mode::never:
        g_enabled = false;
        break;

    case color_mode::automatic:
        // NO_COLOR wins over FORCE_COLOR (the no-color.org convention). Requiring both streams to be
        // terminals means piping either one yields plain output, keeping ANSI out of redirected data.
        if (std::getenv("NO_COLOR") != nullptr)
            g_enabled = false;
        else if (std::getenv("FORCE_COLOR") != nullptr)
            g_enabled = true;
        else
            g_enabled = is_terminal(stdout) && is_terminal(stderr);
        break;
    }

    if (g_enabled)
        enable_windows_vt();
}

bool color_enabled()
{
    return g_enabled;
}

cc::string dim(cc::string_view s)
{
    return wrap("\033[2m", s);
}
cc::string bold(cc::string_view s)
{
    return wrap("\033[1m", s);
}
cc::string red(cc::string_view s)
{
    return wrap("\033[31m", s);
}
cc::string green(cc::string_view s)
{
    return wrap("\033[32m", s);
}
cc::string yellow(cc::string_view s)
{
    return wrap("\033[33m", s);
}
cc::string cyan(cc::string_view s)
{
    return wrap("\033[36m", s);
}
} // namespace itrace
