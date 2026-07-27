#include <clean-core/platform/console.hh>
#include <nexus/test.hh>

using namespace cc::console;

namespace
{
/// The console flag is process-global, so anything that flips it must put it back.
/// Every other test in this binary — and in every tool's — assumes plain output.
struct color_scope
{
    explicit color_scope(color_mode mode) { configure(mode); }
    ~color_scope() { configure(color_mode::never); }
};
} // namespace

TEST("console - never colors")
{
    color_scope const scope(color_mode::never);

    CHECK(!color_enabled());
    CHECK(dim("x") == "x");
    CHECK(bold("x") == "x");
    CHECK(red("x") == "x");
    CHECK(green("x") == "x");
    CHECK(yellow("x") == "x");
    CHECK(cyan("x") == "x");
}

TEST("console - always wraps in ANSI, even when not a terminal")
{
    // The test binary's stdout is redirected by the runner, so auto would say no.
    // An explicit `always` must override that rather than consult the terminal.
    color_scope const scope(color_mode::always);

    CHECK(color_enabled());
    CHECK(dim("x") == "\033[2mx\033[0m");
    CHECK(bold("x") == "\033[1mx\033[0m");
    CHECK(red("x") == "\033[31mx\033[0m");
    CHECK(green("x") == "\033[32mx\033[0m");
    CHECK(yellow("x") == "\033[33mx\033[0m");
    CHECK(cyan("x") == "\033[36mx\033[0m");
}

TEST("console - every color has its own escape")
{
    color_scope const scope(color_mode::always);

    // The attributes and the eight base colors, then the bright variants at +60.
    CHECK(italic("x") == "\033[3mx\033[0m");
    CHECK(underline("x") == "\033[4mx\033[0m");
    CHECK(black("x") == "\033[30mx\033[0m");
    CHECK(blue("x") == "\033[34mx\033[0m");
    CHECK(magenta("x") == "\033[35mx\033[0m");
    CHECK(white("x") == "\033[37mx\033[0m");
    CHECK(bright_black("x") == "\033[90mx\033[0m");
    CHECK(bright_red("x") == "\033[91mx\033[0m");
    CHECK(bright_green("x") == "\033[92mx\033[0m");
    CHECK(bright_yellow("x") == "\033[93mx\033[0m");
    CHECK(bright_blue("x") == "\033[94mx\033[0m");
    CHECK(bright_magenta("x") == "\033[95mx\033[0m");
    CHECK(bright_cyan("x") == "\033[96mx\033[0m");
    CHECK(bright_white("x") == "\033[97mx\033[0m");
}

TEST("console - colorize takes the flag explicitly or from the global")
{
    color_scope const scope(color_mode::never);

    CHECK(colorize(color::red, "x", true) == "\033[31mx\033[0m"); // ignores the global, which is off
    CHECK(colorize(color::red, "x", false) == "x");
    CHECK(colorize(color::red, "x") == "x"); // follows the global
}

TEST("console - auto is off when stdout is not a terminal")
{
    // dev.py captures this binary's output, so auto must resolve to plain here.
    // This is the case that keeps ANSI escapes out of redirected data and CI logs.
    color_scope const scope(color_mode::automatic);
    CHECK(!color_enabled());
}

TEST("console - an empty string round-trips")
{
    color_scope const scope(color_mode::always);
    CHECK(dim("") == "\033[2m\033[0m");
}
