#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/test.hh>
#include <shaped-linter/prose/plan.hh>

using namespace scl;

namespace
{
/// Every edit as `first:last[+]=line|line`, so a whole parse is one comparable string.
cc::string summary(plan_file const& f)
{
    cc::string out;
    for (auto const& e : f.edits)
    {
        if (!out.empty())
            out += ' ';
        out += cc::to_string(e.first_line);
        out += ':';
        out += cc::to_string(e.last_line);
        if (e.is_insertion)
            out += '+';
        out += '=';
        for (isize i = 0; i < e.lines.size(); ++i)
        {
            if (i > 0)
                out += '|';
            out += e.lines[i];
        }
    }
    return out;
}
} // namespace

TEST("prose plan - the three span forms")
{
    auto const plan = parse_prose_plan(R"(## a.hh
[14-17]
| /// one
| /// two
[20]
| /// only
[+52]
| /// inserted
)");

    REQUIRE(plan.has_value());
    REQUIRE(plan.value().files.size() == 1);
    CHECK(plan.value().files[0].path == "a.hh");
    CHECK(summary(plan.value().files[0]) == "14:17=/// one|/// two 20:20=/// only 52:51+=/// inserted");
}

TEST("prose plan - an empty span is a deletion")
{
    auto const plan = parse_prose_plan("## a.hh\n[7-9]\n");

    REQUIRE(plan.has_value());
    REQUIRE(plan.value().files[0].edits.size() == 1);
    CHECK(plan.value().files[0].edits[0].lines.empty());
    CHECK(plan.value().files[0].edits[0].removed_line_count() == 3);
}

TEST("prose plan - the pipe strips exactly one space")
{
    auto const plan = parse_prose_plan("## a.hh\n[1]\n|     /// indented\n|\n| x\n");

    REQUIRE(plan.has_value());
    auto const& lines = plan.value().files[0].edits[0].lines;
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "    /// indented"); // one space eaten, the indentation kept
    CHECK(lines[1] == "");                 // a bare pipe is a blank line inside the block
    CHECK(lines[2] == "x");
}

TEST("prose plan - several files")
{
    auto const plan = parse_prose_plan("## a.hh\n[1]\n| x\n\n## b/c.md\n[2-3]\n| y\n");

    REQUIRE(plan.has_value());
    REQUIRE(plan.value().files.size() == 2);
    CHECK(plan.value().files[0].path == "a.hh");
    CHECK(plan.value().files[1].path == "b/c.md");
}

TEST("prose plan - an insertion may sit directly before a replacement of the same line")
{
    auto const plan = parse_prose_plan("## a.hh\n[+52]\n| x\n[52-53]\n| y\n");
    CHECK(plan.has_value());
}

TEST("prose plan - malformed input is rejected")
{
    SECTION("spans must ascend")
    {
        CHECK(parse_prose_plan("## a.hh\n[10-12]\n| x\n[5]\n| y\n").has_error());
    }
    SECTION("spans must not overlap")
    {
        CHECK(parse_prose_plan("## a.hh\n[10-12]\n| x\n[12-14]\n| y\n").has_error());
    }
    SECTION("a range must not run backwards")
    {
        CHECK(parse_prose_plan("## a.hh\n[9-4]\n| x\n").has_error());
    }
    SECTION("line numbers are 1-based")
    {
        CHECK(parse_prose_plan("## a.hh\n[0]\n| x\n").has_error());
    }
    SECTION("a span needs its bracket")
    {
        CHECK(parse_prose_plan("## a.hh\n[1\n| x\n").has_error());
    }
    SECTION("a span needs a file")
    {
        CHECK(parse_prose_plan("[1]\n| x\n").has_error());
    }
    SECTION("a replacement needs a span")
    {
        CHECK(parse_prose_plan("## a.hh\n| x\n").has_error());
    }
    SECTION("a file may not repeat")
    {
        CHECK(parse_prose_plan("## a.hh\n[1]\n| x\n## a.hh\n[2]\n| y\n").has_error());
    }
    SECTION("a header needs a path")
    {
        CHECK(parse_prose_plan("## \n").has_error());
    }
    SECTION("a stray line is not silently skipped")
    {
        CHECK(parse_prose_plan("## a.hh\noops\n[1]\n| x\n").has_error());
    }
    SECTION("a line number must be digits")
    {
        CHECK(parse_prose_plan("## a.hh\n[x-3]\n| x\n").has_error());
    }
}

TEST("prose plan - an empty plan parses to nothing")
{
    auto const plan = parse_prose_plan("\n\n");

    REQUIRE(plan.has_value());
    CHECK(plan.value().files.empty());
}

TEST("prose plan - CRLF reads the same as LF")
{
    auto const plan = parse_prose_plan("## a.hh\r\n[1]\r\n| /// x\r\n");

    REQUIRE(plan.has_value());
    CHECK(plan.value().files[0].path == "a.hh");
    CHECK(plan.value().files[0].edits[0].lines[0] == "/// x");
}
