#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `no-long-prose-line` — the scratchpad the rule was built in, and where an interesting
// regression gets pinned.
// Breadth lives in no_long_prose_line.md, next to this file.

using namespace scl;

namespace
{
/// A prose line exactly `chars` characters wide, of short words so it stays splittable.
/// It never ends on a space, which the extractor would trim back out of the measurement.
cc::string line_of_width(cc::string_view marker, isize chars)
{
    auto s = cc::string::create_filled(chars, 'x');
    for (isize i = marker.size() + 1; i < chars - 1; i += 5) // word breaks, but never the first or last char
        s[i] = ' ';
    for (isize i = 0; i < marker.size(); ++i)
        s[i] = marker[i];
    s += cc::string_view("\n");
    return s;
}

isize count_in(cc::string_view source, cc::string_view path)
{
    auto const found = run_rules_on_text(source, path);
    for (auto const& f : found)
        CHECK(f.rule_id == "no-long-prose-line");
    return found.size();
}
} // namespace

TEST("shaped-linter - no-long-prose-line - the ceiling")
{
    SECTION("a line at the ceiling is fine")
    {
        CHECK(count_in(line_of_width("// ", 200), "a.cc") == 0);
    }
    SECTION("one character over fires")
    {
        CHECK(count_in(line_of_width("// ", 201), "a.cc") == 1);
    }
    SECTION("a comfortable line is nowhere near it")
    {
        CHECK(count_in("// one point, written out plainly\n", "a.cc") == 0);
    }
    SECTION("each over-long line is its own finding")
    {
        CHECK(count_in(line_of_width("// ", 201) + line_of_width("// ", 220), "a.cc") == 2);
    }
}

TEST("shaped-linter - no-long-prose-line - characters, not bytes")
{
    // Every em dash is three UTF-8 bytes, so 30 of them cost 60 bytes beyond their 30 characters.
    // The line below is 199 characters and 259 bytes: a byte count would wrongly fire on it.
    auto s = cc::string("// ");
    for (auto i = 0; i < 30; ++i)
        s += cc::string_view("—");
    while (s.size() < 199 + 2 * 30)
        s += cc::string_view("x");
    s += cc::string_view("\n");

    CHECK(count_in(s, "a.cc") == 0);
}

TEST("shaped-linter - no-long-prose-line - what it leaves alone")
{
    SECTION("an unsplittable run over the ceiling is not a wrapping mistake")
    {
        auto s = cc::string("// see https://example.com/");
        for (auto i = 0; i < 210; ++i)
            s += cc::string_view("a");
        s += cc::string_view("\n");
        CHECK(count_in(s, "a.cc") == 0);
    }
    SECTION("but a long line of ordinary words is")
    {
        CHECK(count_in(line_of_width("// ", 240), "a.cc") == 1);
    }
    SECTION("code is not prose, however long the line")
    {
        auto s = cc::string("auto const s = \"");
        for (auto i = 0; i < 260; ++i)
            s += cc::string_view("a");
        s += cc::string_view("\";\n");
        CHECK(count_in(s, "a.cc") == 0);
    }
}

TEST("shaped-linter - no-long-prose-line - every language it binds")
{
    SECTION("a Python comment")
    {
        CHECK(count_in(line_of_width("# ", 210), "a.py") == 1);
    }
    SECTION("markdown body text")
    {
        CHECK(count_in(line_of_width("", 210), "a.md") == 1);
    }
}

TEST("shaped-linter - no-long-prose-line - what a finding carries")
{
    auto const found = run_rules_on_text(line_of_width("// ", 210), "a.cc");
    REQUIRE(found.size() == 1);

    // The carets underline the overhang alone, so they show exactly how much is over the ceiling.
    CHECK(found[0].span.byte_end - found[0].span.byte_begin == 10);

    // No fix: splitting the line mechanically is not what obeying the rule means.
    CHECK(!found[0].suggested_fix.has_value());
    REQUIRE(found[0].suggested_hint.has_value());
    CHECK(found[0].suggested_hint.value().edits.size() == 0);
    CHECK(found[0].suggested_hint.value().message.contains("coding-guidelines.md"));
}
