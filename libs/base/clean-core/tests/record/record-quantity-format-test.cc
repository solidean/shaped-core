#include <clean-core/platform/environment.hh>
#include <clean-core/record/console_listener.hh>
#include <clean-core/record/quantity_format.hh>
#include <clean-core/record/stat.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// prefix_base, symbol, singular and plural were carried, serialized and asserted on for a long time before
// anything rendered a value through them.
// These pin what that rendering actually says.

TEST("rec/quantity - bytes take binary prefixes")
{
    CHECK(cc::rec::format_quantity(0, cc::rec::unit_bytes) == "0.00 B");
    CHECK(cc::rec::format_quantity(512, cc::rec::unit_bytes) == "512 B");
    CHECK(cc::rec::format_quantity(1024, cc::rec::unit_bytes) == "1.00 KiB");
    CHECK(cc::rec::format_quantity(1536, cc::rec::unit_bytes) == "1.50 KiB");
    CHECK(cc::rec::format_quantity(1024.0 * 1024 * 1024, cc::rec::unit_bytes) == "1.00 GiB");

    // 1000 is not a kilobyte here, which is the entire point of prefix_base being 1024 for this unit.
    CHECK(cc::rec::format_quantity(1000, cc::rec::unit_bytes) == "1000 B");
}

TEST("rec/quantity - seconds scale down as well as up")
{
    CHECK(cc::rec::format_quantity(1, cc::rec::unit_seconds) == "1.00 s");
    CHECK(cc::rec::format_quantity(0.005, cc::rec::unit_seconds) == "5.00 ms");
    CHECK(cc::rec::format_quantity(0.000012, cc::rec::unit_seconds) == "12.0 us");
    CHECK(cc::rec::format_quantity(1500, cc::rec::unit_seconds) == "1.50 ks");
}

TEST("rec/quantity - hertz takes SI prefixes")
{
    CHECK(cc::rec::format_quantity(3.2e9, cc::rec::unit_hertz) == "3.20 GHz");
    CHECK(cc::rec::format_quantity(60, cc::rec::unit_hertz) == "60.0 Hz");
}

TEST("rec/quantity - a unit with no symbol falls back to its own name")
{
    CHECK(cc::rec::format_quantity(42, cc::rec::unit_count) == "42.0 items");

    // Exactly one is singular, which is the only reason the unit carries both spellings.
    CHECK(cc::rec::format_quantity(1, cc::rec::unit_count) == "1.00 item");
}

TEST("rec/quantity - a bare quantity prints as a bare number")
{
    // No symbol and no prefix: "0.75 ratios" would be worse than "0.75".
    CHECK(cc::rec::format_quantity(0.75, cc::rec::unit_ratio) == "0.75");
    CHECK(cc::rec::format_quantity(1, cc::rec::unit_ratio) == "1.00");
}

TEST("rec/quantity - negative values keep their sign and their prefix")
{
    CHECK(cc::rec::format_quantity(-1536, cc::rec::unit_bytes) == "-1.50 KiB");
}

TEST("rec/quantity - a unit may override the generic rule")
{
    // The escape hatch for the units the generic rule gets wrong — a ratio a human reads as a percentage.
    static constexpr auto percent = cc::rec::unit{
        .singular = "percent",
        .plural = "percent",
        .symbol = "%",
        .prefix_base = 0,
        .format = [](cc::string& out, f64 value, cc::rec::unit const&) { out.appendf("{:.1f}%", value * 100); },
    };

    CHECK(cc::rec::format_quantity(0.42, percent) == "42.0%");
}

TEST("rec/quantity - the console listener opts stats in from the environment")
{
    // show_stats is off by default, and CC_LOG_STATS is how someone turns it on in a program they cannot rebuild --
    // the same escape hatch every other console option has.
    CHECK(!cc::rec::console_options().show_stats);

    {
        auto const on = cc::scoped_environment_variable("CC_LOG_STATS", "1");
        CHECK(cc::rec::console_options::from_environment().show_stats);
    }

    {
        auto const off = cc::scoped_environment_variable("CC_LOG_STATS", "0");
        CHECK(!cc::rec::console_options::from_environment().show_stats);
    }
}
