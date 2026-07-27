#include <nexus/test.hh>
#include <shaped-linter/report/impl/display_width.hh>

using namespace scl::impl;

TEST("shaped-linter - display_width - tabs advance to the next stop")
{
    CHECK(expand_tabs("\tx", 4) == "    x");
    CHECK(expand_tabs("a\tx", 4) == "a   x"); // 1 -> 4
    CHECK(expand_tabs("abcd\tx", 4) == "abcd    x");
    CHECK(expand_tabs("no tabs", 4) == "no tabs");
}

TEST("shaped-linter - display_width - columns follow the expansion")
{
    CHECK(display_column("\tx", 0, 4) == 0);
    CHECK(display_column("\tx", 1, 4) == 4); // 'x' sits at the first tab stop
    CHECK(display_column("a\tx", 2, 4) == 4);
}

TEST("shaped-linter - display_width - a codepoint is one column, however many bytes it takes")
{
    // "héllo": 'é' is two bytes, so byte 3 is the second 'l'... and column 2 is the first 'l'.
    auto const line = cc::string_view("héllo");
    CHECK(line.size() == 6);
    CHECK(display_column(line, 3, 4) == 2);
    CHECK(display_column(line, line.size(), 4) == 5);
}

TEST("shaped-linter - display_width - offsets are clamped")
{
    CHECK(display_column("abc", -5, 4) == 0);
    CHECK(display_column("abc", 99, 4) == 3);
}
