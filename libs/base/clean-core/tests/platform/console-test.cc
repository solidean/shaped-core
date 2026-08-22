#include <clean-core/platform/console.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/string.hh>
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

// Restoring matters here: a leaked NO_COLOR would silently disable color for every later test in this binary.
using cc::scoped_environment_variable;

// Every test here writes process-wide state — the global color mode, and NO_COLOR / FORCE_COLOR / COLUMNS in the environment — so they share one exclusion tag.
TEST("console - never colors", exclusive("cc-console-color"))
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

TEST("console - always wraps in ANSI, even when not a terminal", exclusive("cc-console-color"))
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

TEST("console - every color has its own escape", exclusive("cc-console-color"))
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

TEST("console - colorize takes the flag explicitly or from the global", exclusive("cc-console-color"))
{
    color_scope const scope(color_mode::never);

    CHECK(colorize(color::red, "x", true) == "\033[31mx\033[0m"); // ignores the global, which is off
    CHECK(colorize(color::red, "x", false) == "x");
    CHECK(colorize(color::red, "x") == "x"); // follows the global
}

// Whether `automatic` lands on colored depends on the runner's stdio, which a test cannot pin — under
// Emscripten's node shim, for one, a captured stdout still reports as a terminal.
// So these pin the two rules that hold whatever the stdio looks like.

TEST("console - NO_COLOR forces plain and beats FORCE_COLOR", exclusive("cc-console-color"))
{
    scoped_environment_variable const no_color("NO_COLOR", "1");
    scoped_environment_variable const force_color("FORCE_COLOR", "1");
    color_scope const scope(color_mode::automatic);

    CHECK(!color_enabled());
}

TEST("console - FORCE_COLOR colors a stream that is not a terminal", exclusive("cc-console-color"))
{
    scoped_environment_variable const no_color("NO_COLOR", ""); // empty is how this API spells "unset"
    scoped_environment_variable const force_color("FORCE_COLOR", "1");
    color_scope const scope(color_mode::automatic);

    CHECK(color_enabled());
}

TEST("console - an explicit mode ignores the environment entirely", exclusive("cc-console-color"))
{
    scoped_environment_variable const force_color("FORCE_COLOR", "1");
    color_scope const scope(color_mode::never);

    CHECK(!color_enabled());
}

TEST("console - an empty string round-trips", exclusive("cc-console-color"))
{
    color_scope const scope(color_mode::always);
    CHECK(dim("") == "\033[2m\033[0m");
}

// Whether a real terminal is behind stdout is not something a test can pin, so these cover the COLUMNS
// override — the path that exists precisely so wrapped output can be tested without one.

TEST("console - COLUMNS overrides the terminal", exclusive("cc-console-color"))
{
    scoped_environment_variable const columns("COLUMNS", "37");

    auto const width = terminal_width();
    REQUIRE(width.has_value());
    CHECK(width.value() == 37);
}

TEST("console - a COLUMNS that is not a positive number is ignored", exclusive("cc-console-color"))
{
    // Each falls through to the real terminal, which may or may not answer; what must not happen is 0 or a negative width.
    for (auto const* const bad : {"0", "-5", "wide", "", "80x24", "80 "})
    {
        scoped_environment_variable const columns("COLUMNS", bad);

        auto const width = terminal_width();
        CHECK((!width.has_value() || width.value() > 0)); // the extra parens keep CHECK from decomposing the ||
    }
}

TEST("console - width is independent of the color flag", exclusive("cc-console-color"))
{
    scoped_environment_variable const columns("COLUMNS", "120");

    color_scope const scope(color_mode::never);
    CHECK(terminal_width().value_or(0) == 120);
}
