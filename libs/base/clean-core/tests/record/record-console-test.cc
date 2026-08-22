#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/record/console_listener.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// The console listener is the one part of cc::rec a person reads directly, so what it puts on a line is API.
//
// These pin the OPTIONS and the resolution rules rather than the exact glyphs: the layout is meant to be tuned, and
// a test that pins every space would make tuning it a chore rather than a decision.

TEST("record/console - explicit options are taken verbatim, ignoring the environment")
{
    // The contract that lets an application configure its logging and keep it: only a DEFAULT-constructed listener
    // consults the environment, so a stray CC_LOG_LEVEL in a developer's shell cannot silence a program that asked
    // for debug output in code.
    auto const listener = cc::rec::console_listener({
        .min_level = cc::rec::level::debug,
        .time = cc::rec::console_time::none,
        .show_thread = false,
        .color = cc::console::color_mode::never,
    });

    CHECK(listener.options().min_level == cc::rec::level::debug);
    CHECK(listener.options().time == cc::rec::console_time::none);
    CHECK(!listener.options().show_thread);
    CHECK(!listener.is_colored());
}

TEST("record/console - color::always resolves to colored whether or not a terminal is attached")
{
    // A test binary's stdout is normally a pipe, so `automatic` resolves to plain here and says nothing useful.
    // The two explicit modes are what a caller can rely on, and they are what this pins.
    CHECK(cc::rec::console_listener({.color = cc::console::color_mode::always}).is_colored());
    CHECK(!cc::rec::console_listener({.color = cc::console::color_mode::never}).is_colored());
}

TEST("record/console - the defaults are the ones an application wants")
{
    auto const defaults = cc::rec::console_options{};

    CHECK(defaults.min_level == cc::rec::level::info);
    CHECK(defaults.time == cc::rec::console_time::wall_time); // a stale terminal is then obvious at a glance
    CHECK(defaults.show_domain);
    CHECK(defaults.split_streams);


    // Source locations are off even for errors: a .ccrec carries them for every event, offline and exactly.
    CHECK(!defaults.show_site);
}

REC_TEST("record/console - a plain listener prints one line per message at or above its level")
{
    rec_fixture const fixture(deterministic_config());

    auto console = cc::rec::console_listener({
        .min_level = cc::rec::level::warning,
        .time = cc::rec::console_time::none,
        .color = cc::console::color_mode::never,
    });

    {
        scoped_listener const reg(console);

        CC_LOG_INFO("below the threshold");
        CC_RECORD_MARK("not-a-log");
        CC_LOG_WARNING("printed");
        CC_LOG_ERROR("printed too");
        cc::rec::flush_blocking();
    }

    CHECK(console.printed_count() == 2);
}

//
// The local calendar breakdown the wall-clock modes are built on
//

TEST("time/local_calendar_time - the epoch comes back as a real date")
{
    // 2001-09-09T01:46:40Z, the "one billion seconds" instant.
    // The local hour depends on the machine's zone, so what is pinned is the fields being in range and the
    // sub-second part surviving — not a specific hour, which would fail everywhere but one timezone.
    auto const t = cc::local_calendar_time(1'000'000'000.25);

    CHECK(t.year == 2001);
    CHECK(t.month == 9);
    CHECK(t.hour < 24);
    CHECK(t.minute < 60);
    CHECK(t.second <= 60);
    CHECK(t.millisecond == 250);
}

TEST("time/local_calendar_time - now is this century")
{
    auto const t = cc::local_calendar_time(cc::current_time_wall_secs());

    CHECK(t.year >= 2024);
    CHECK(t.year < 2200);
    CHECK(t.month >= 1);
    CHECK(t.month <= 12);
    CHECK(t.day >= 1);
    CHECK(t.day <= 31);
    CHECK(t.millisecond < 1000);
}

//
// A look at the real thing
//

/// Prints one line in each time mode, colored and plain, so the layout can be judged by eye rather than by diff.
/// Manual: it asserts nothing, and what it is for is looking at it.
///
///   uv run dev.py --mirror-test-output test "record/console - sample lines" --manual
TEST("record/console - sample lines in every mode", nx::config::manual, nx::config::exclusive(), nx::config::owns_recorder)
{
    struct variant
    {
        char const* what;
        cc::rec::console_options options;
    };

    variant const variants[] = {
        {"none    ", {.time = cc::rec::console_time::none, .color = cc::console::color_mode::never}},
        {"elapsed ", {.time = cc::rec::console_time::elapsed, .color = cc::console::color_mode::never}},
        {"time    ", {.time = cc::rec::console_time::wall_time, .color = cc::console::color_mode::never}},
        {"datetime", {.time = cc::rec::console_time::wall_datetime, .color = cc::console::color_mode::never}},
        {"colored ", {.time = cc::rec::console_time::wall_time, .color = cc::console::color_mode::always}},
        {"with-site",
         {.time = cc::rec::console_time::wall_time, .show_site = true, .color = cc::console::color_mode::never}},
    };

    for (auto const& v : variants)
    {
        cc::println("");
        cc::println("--- {} ---", v.what);

        rec_fixture const fixture(deterministic_config());
        auto listener = cc::rec::console_listener(v.options);
        scoped_listener const reg(listener);

        CC_LOG_INFO("an ordinary message");
        CC_LOG_WARNING("something looks off: {} of {}", 3, 7);
        CC_LOG_ERROR("and something failed");
        cc::rec::flush_blocking();
    }
}
