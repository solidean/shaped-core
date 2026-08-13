#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

enum class cc::console::color_mode
{
    automatic,
    always, // an explicit --colored
    never,  // an explicit --plain
};

/// The SGR set every terminal agrees on: four attributes, the eight base colors, and their bright variants.
/// 256-color and true-color are out of scope — they need a capability database, and nothing here does.
enum class cc::console::color : cc::u8
{
    // attributes
    bold,
    dim,
    italic,
    underline,

    // the eight base colors (SGR 30-37)
    black,
    red,
    green,
    yellow,
    blue,
    magenta,
    cyan,
    white,

    // their bright variants (SGR 90-97); bright_black is the usual "gray"
    bright_black,
    bright_red,
    bright_green,
    bright_yellow,
    bright_blue,
    bright_magenta,
    bright_cyan,
    bright_white,
};

namespace cc::console
{

/// Resolve whether output is colored.
/// Call once, before the first byte of output — including usage errors, so a failing run is styled like a succeeding one.
/// NO_COLOR (any value) forces off and beats FORCE_COLOR; FORCE_COLOR forces on.
/// Otherwise color is on only when stdout and stderr are BOTH terminals: piping either one yields plain output, which is what keeps ANSI out of redirected data and CI logs.
/// Also enables ANSI processing on Windows consoles that still need to be told.
void configure(color_mode mode);

/// False until `configure` runs, so a process that never configures prints plain.
bool color_enabled();

/// Wrap `text` in `c`'s escape when `enabled`, and return it unchanged otherwise.
/// The form for a renderer that carries its own color flag: a pure function stays pure, and its tests do not depend on how the process was invoked.
cc::string colorize(color c, cc::string_view text, bool enabled);

/// The same, driven by the process-global flag `configure` resolved.
cc::string colorize(color c, cc::string_view text);

// One named helper per `color`, all shorthands for `colorize(color::x, s)`.
// Each returns its argument unchanged when color is off, so call sites read the same either way.
// The flag is process-global: a test that flips it must put it back.
cc::string bold(cc::string_view s);
cc::string dim(cc::string_view s);
cc::string italic(cc::string_view s);
cc::string underline(cc::string_view s);

cc::string black(cc::string_view s);
cc::string red(cc::string_view s);
cc::string green(cc::string_view s);
cc::string yellow(cc::string_view s);
cc::string blue(cc::string_view s);
cc::string magenta(cc::string_view s);
cc::string cyan(cc::string_view s);
cc::string white(cc::string_view s);

cc::string bright_black(cc::string_view s);
cc::string bright_red(cc::string_view s);
cc::string bright_green(cc::string_view s);
cc::string bright_yellow(cc::string_view s);
cc::string bright_blue(cc::string_view s);
cc::string bright_magenta(cc::string_view s);
cc::string bright_cyan(cc::string_view s);
cc::string bright_white(cc::string_view s);
} // namespace cc::console
