#include "console.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/string/format.hh>
#include <stdio.h>

#include <cstdlib>

#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace cc::console
{
namespace
{
constexpr cc::string_view reset = "\033[0m";

bool g_enabled = false;

bool is_terminal(FILE* stream)
{
#ifdef CC_OS_WINDOWS
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

/// Old conhost needs to be told it speaks ANSI; Windows Terminal already does.
/// Failure is fine: it only means we are not on a VT-capable console, and the caller's isatty check already gates us.
void enable_windows_vt()
{
#ifdef CC_OS_WINDOWS
    auto* const handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return;

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode))
        return;

    SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

cc::string_view escape_of(color c)
{
    switch (c)
    {
    case color::bold:
        return "\033[1m";
    case color::dim:
        return "\033[2m";
    case color::italic:
        return "\033[3m";
    case color::underline:
        return "\033[4m";

    case color::black:
        return "\033[30m";
    case color::red:
        return "\033[31m";
    case color::green:
        return "\033[32m";
    case color::yellow:
        return "\033[33m";
    case color::blue:
        return "\033[34m";
    case color::magenta:
        return "\033[35m";
    case color::cyan:
        return "\033[36m";
    case color::white:
        return "\033[37m";

    case color::bright_black:
        return "\033[90m";
    case color::bright_red:
        return "\033[91m";
    case color::bright_green:
        return "\033[92m";
    case color::bright_yellow:
        return "\033[93m";
    case color::bright_blue:
        return "\033[94m";
    case color::bright_magenta:
        return "\033[95m";
    case color::bright_cyan:
        return "\033[96m";
    case color::bright_white:
        return "\033[97m";
    }
    return "";
}
} // namespace

void configure(color_mode mode)
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
        // NO_COLOR wins over FORCE_COLOR (the no-color.org convention).
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

cc::string colorize(color c, cc::string_view text, bool enabled)
{
    if (!enabled)
        return cc::string(text);

    return cc::format("{}{}{}", escape_of(c), text, reset);
}

cc::string colorize(color c, cc::string_view text)
{
    return colorize(c, text, g_enabled);
}

cc::string bold(cc::string_view s)
{
    return colorize(color::bold, s);
}
cc::string dim(cc::string_view s)
{
    return colorize(color::dim, s);
}
cc::string italic(cc::string_view s)
{
    return colorize(color::italic, s);
}
cc::string underline(cc::string_view s)
{
    return colorize(color::underline, s);
}

cc::string black(cc::string_view s)
{
    return colorize(color::black, s);
}
cc::string red(cc::string_view s)
{
    return colorize(color::red, s);
}
cc::string green(cc::string_view s)
{
    return colorize(color::green, s);
}
cc::string yellow(cc::string_view s)
{
    return colorize(color::yellow, s);
}
cc::string blue(cc::string_view s)
{
    return colorize(color::blue, s);
}
cc::string magenta(cc::string_view s)
{
    return colorize(color::magenta, s);
}
cc::string cyan(cc::string_view s)
{
    return colorize(color::cyan, s);
}
cc::string white(cc::string_view s)
{
    return colorize(color::white, s);
}

cc::string bright_black(cc::string_view s)
{
    return colorize(color::bright_black, s);
}
cc::string bright_red(cc::string_view s)
{
    return colorize(color::bright_red, s);
}
cc::string bright_green(cc::string_view s)
{
    return colorize(color::bright_green, s);
}
cc::string bright_yellow(cc::string_view s)
{
    return colorize(color::bright_yellow, s);
}
cc::string bright_blue(cc::string_view s)
{
    return colorize(color::bright_blue, s);
}
cc::string bright_magenta(cc::string_view s)
{
    return colorize(color::bright_magenta, s);
}
cc::string bright_cyan(cc::string_view s)
{
    return colorize(color::bright_cyan, s);
}
cc::string bright_white(cc::string_view s)
{
    return colorize(color::bright_white, s);
}
} // namespace cc::console
