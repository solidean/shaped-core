#include <instruction-tracer/report/console.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
/// The console flag is global, so anything that flips it must put it back — the formatter tests
/// next door assume plain output.
struct color_scope
{
    explicit color_scope(color_mode mode) { configure_console(mode); }
    ~color_scope() { configure_console(color_mode::never); }
};
} // namespace

TEST("console - --plain never colors")
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

TEST("console - --colored wraps in ANSI, even when not a terminal")
{
    // The test binary's stdout is redirected by the runner, so auto would say no; --colored must
    // override that rather than consult the terminal.
    color_scope const scope(color_mode::always);

    CHECK(color_enabled());
    CHECK(dim("x") == "\033[2mx\033[0m");
    CHECK(bold("x") == "\033[1mx\033[0m");
    CHECK(red("x") == "\033[31mx\033[0m");
    CHECK(green("x") == "\033[32mx\033[0m");
    CHECK(yellow("x") == "\033[33mx\033[0m");
    CHECK(cyan("x") == "\033[36mx\033[0m");
}

TEST("console - auto is off when stdout is not a terminal")
{
    // dev.py captures this binary's output, so auto must resolve to plain here. This is the case
    // that keeps ANSI escapes out of redirected data and CI logs.
    color_scope const scope(color_mode::automatic);
    CHECK(!color_enabled());
}

TEST("console - an empty string round-trips")
{
    color_scope const scope(color_mode::always);
    CHECK(dim("") == "\033[2m\033[0m");
}
