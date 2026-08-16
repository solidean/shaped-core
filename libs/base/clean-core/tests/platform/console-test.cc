#include <clean-core/common/macros.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

#include <cstdlib> // getenv / setenv / _putenv_s

#ifndef CC_OS_WINDOWS
#include <unistd.h>
#endif

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

/// `value == nullptr` removes the variable.
void set_env(char const* name, char const* value)
{
#ifdef CC_OS_WINDOWS
    _putenv_s(name, value == nullptr ? "" : value); // an empty value removes it
#else
    if (value == nullptr)
        unsetenv(name);
    else
        setenv(name, value, 1);
#endif
}

/// One environment variable, set for the duration of a test and restored after it.
/// Restoring matters: a leaked NO_COLOR would silently disable color for every later test in this binary.
struct env_scope
{
    env_scope(char const* name, char const* value) : _name(name)
    {
        auto const* const previous = std::getenv(name);
        _had_previous = previous != nullptr;
        if (_had_previous)
            _previous = cc::string::create_copy_c_str_materialized(previous);

        set_env(name, value);
    }

    ~env_scope() { set_env(_name, _had_previous ? _previous.c_str_if_terminated() : nullptr); }

    env_scope(env_scope const&) = delete;
    env_scope& operator=(env_scope const&) = delete;

private:
    char const* _name;
    cc::string _previous;
    bool _had_previous = false;
};
} // namespace

// Every test here writes process-wide state — the global color mode, and NO_COLOR / FORCE_COLOR in the environment — so they share one exclusion tag.
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
    env_scope const no_color("NO_COLOR", "1");
    env_scope const force_color("FORCE_COLOR", "1");
    color_scope const scope(color_mode::automatic);

    CHECK(!color_enabled());
}

TEST("console - FORCE_COLOR colors a stream that is not a terminal", exclusive("cc-console-color"))
{
    env_scope const no_color("NO_COLOR", nullptr);
    env_scope const force_color("FORCE_COLOR", "1");
    color_scope const scope(color_mode::automatic);

    CHECK(color_enabled());
}

TEST("console - an explicit mode ignores the environment entirely", exclusive("cc-console-color"))
{
    env_scope const force_color("FORCE_COLOR", "1");
    color_scope const scope(color_mode::never);

    CHECK(!color_enabled());
}

TEST("console - an empty string round-trips", exclusive("cc-console-color"))
{
    color_scope const scope(color_mode::always);
    CHECK(dim("") == "\033[2m\033[0m");
}
