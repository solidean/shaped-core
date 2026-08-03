#include <nexus/test.hh>
#include <shaped-linter/cli/changed_lines.hh>

using namespace scl;

TEST("changed lines - ranges are inclusive on both ends")
{
    auto const scope = parse_changed_lines("libs/a.hh:10-12\n");
    REQUIRE(scope.has_value());

    CHECK(!scope.value().empty());
    CHECK(!scope.value().covers("libs/a.hh", 9));
    CHECK(scope.value().covers("libs/a.hh", 10));
    CHECK(scope.value().covers("libs/a.hh", 12));
    CHECK(!scope.value().covers("libs/a.hh", 13));
}

TEST("changed lines - several ranges per file, several files")
{
    auto const scope = parse_changed_lines("a.hh:1-2,7-7\nb.md:5-5\n");
    REQUIRE(scope.has_value());

    CHECK(scope.value().covers("a.hh", 2));
    CHECK(!scope.value().covers("a.hh", 5));
    CHECK(scope.value().covers("a.hh", 7));
    CHECK(scope.value().covers("b.md", 5));
}

TEST("changed lines - a file with no entry has no reportable lines")
{
    auto const scope = parse_changed_lines("a.hh:1-2\n");
    REQUIRE(scope.has_value());

    CHECK(!scope.value().covers("other.hh", 1));
}

TEST("changed lines - separators are normalized")
{
    auto const scope = parse_changed_lines("libs/base/a.hh:3-3\n");
    REQUIRE(scope.has_value());

    CHECK(scope.value().covers("libs\\base\\a.hh", 3));
}

TEST("changed lines - a windows drive colon is not the separator")
{
    auto const scope = parse_changed_lines("C:/Projects/a.hh:4-5\n");
    REQUIRE(scope.has_value());

    CHECK(scope.value().covers("C:/Projects/a.hh", 4));
}

TEST("changed lines - a repeated path unions its ranges")
{
    auto const scope = parse_changed_lines("a.hh:1-2\na.hh:8-9\n");
    REQUIRE(scope.has_value());

    CHECK(scope.value().covers("a.hh", 1));
    CHECK(scope.value().covers("a.hh", 9));
}

TEST("changed lines - an empty spec filters nothing")
{
    auto const scope = parse_changed_lines("");
    REQUIRE(scope.has_value());

    CHECK(scope.value().empty());
}

TEST("changed lines - malformed input is rejected")
{
    SECTION("no ranges")
    {
        CHECK(parse_changed_lines("a.hh\n").has_error());
    }
    SECTION("no path")
    {
        CHECK(parse_changed_lines(":1-2\n").has_error());
    }
    SECTION("not a number")
    {
        CHECK(parse_changed_lines("a.hh:x-2\n").has_error());
    }
}
