#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace itrace
{
enum class color_mode
{
    automatic,
    always, // --colored
    never,  // --plain
};

/// Resolve whether output is colored, once, at startup. Mirrors dev.py's policy (see
/// tools/dev/lib/core/console.py) so both tools behave the same in a pipe or a CI log: NO_COLOR
/// (any value) forces off and beats FORCE_COLOR, FORCE_COLOR forces on, otherwise color is on only
/// when stdout and stderr are both terminals. Also enables ANSI processing on Windows consoles that
/// still need it.
void configure_console(color_mode mode);

bool color_enabled();

// Style helpers. Each returns its argument unchanged when color is off, so call sites read the same
// either way. Colors are off until configure_console runs — which is what keeps the formatter tests
// comparing plain text.
cc::string dim(cc::string_view s);
cc::string bold(cc::string_view s);
cc::string red(cc::string_view s);
cc::string green(cc::string_view s);
cc::string yellow(cc::string_view s);
cc::string cyan(cc::string_view s);
} // namespace itrace
