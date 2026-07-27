#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/report/snippet.hh>

using namespace scl;

namespace
{
/// One in-memory file, rendered with one or more labels.
/// Color stays off, so the expectations below are the visible layout and nothing else.
cc::string render(cc::string_view text, cc::vector<label> labels, report_style style = {})
{
    source_manager sm;
    sm.add_from_text(cc::string(text), "x.hh");
    return render_snippet(labels, sm, style);
}

label at(u32 begin, u32 end, cc::string_view text = "", u32 file_id = 0)
{
    return {.span = {.file_id = file_id, .byte_begin = begin, .byte_end = end}, .text = cc::string(text)};
}
} // namespace

TEST("shaped-linter - snippet - a single-line span with context")
{
    auto const out = render("aaa\nbbb\nccc\nddd\neee", {at(8, 11)});

    CHECK(out == R"( --> x.hh:3:1
  |
1 | aaa
2 | bbb
3 | ccc
  | ^^^
4 | ddd
5 | eee
  |
)");
}

TEST("shaped-linter - snippet - context clamps at the start and end of the file")
{
    auto const out = render("aaa\nbbb\nccc", {at(0, 3)});

    CHECK(out == R"( --> x.hh:1:1
  |
1 | aaa
  | ^^^
2 | bbb
3 | ccc
  |
)");
}

TEST("shaped-linter - snippet - an empty span still points somewhere")
{
    auto const out = render("aaa", {at(1, 1)});

    CHECK(out == R"( --> x.hh:1:2
  |
1 | aaa
  |  ^
  |
)");
}

TEST("shaped-linter - snippet - a label prints next to its underline")
{
    auto const out = render("aaa", {at(0, 3, "here")});

    CHECK(out == R"( --> x.hh:1:1
  |
1 | aaa
  | ^^^ here
  |
)");
}

TEST("shaped-linter - snippet - a multi-line span underlines each of its lines")
{
    auto const out = render("aaa\nbbb\nccc", {at(1, 5)});

    CHECK(out == R"( --> x.hh:1:2
  |
1 | aaa
  |  ^^
2 | bbb
  | ^
3 | ccc
  |
)");
}

TEST("shaped-linter - snippet - a long span has its middle elided")
{
    auto text = cc::string(); // exactly 10 lines, no trailing newline
    for (auto i = 1; i <= 10; ++i)
    {
        if (i > 1)
            text += '\n';
        text += cc::format("L{:02}", i);
    }

    // Lines 2..9 is more than max_span_lines, so only the first and last two are underlined.
    // With one line of context the untouched middle collapses to a `...` row.
    auto const out = render(text, {at(4, 35)}, {.context_lines = 1});

    CHECK(out == R"(  --> x.hh:2:1
   |
 1 | L01
 2 | L02
   | ^^^
 3 | L03
   | ^^^
 4 | L04
...
 7 | L07
 8 | L08
   | ^^^
 9 | L09
   | ^^^
10 | L10
   |
)");
}

TEST("shaped-linter - snippet - two labels on one line become a ladder")
{
    auto const out = render("aaa bbb", {at(0, 3, "first"), at(4, 7, "second")});

    CHECK(out == R"( --> x.hh:1:1
  |
1 | aaa bbb
  | ^^^ ---
  | |   |
  | |   second
  | first
  |
)");
}

TEST("shaped-linter - snippet - carets land under tab-indented code")
{
    auto const out = render("\tint a;", {at(1, 4)});

    CHECK(out == R"( --> x.hh:1:2
  |
1 |     int a;
  |     ^^^
  |
)");
}

TEST("shaped-linter - snippet - a secondary label in another file gets its own block")
{
    source_manager sm;
    sm.add_from_text("aaa", "a.hh");
    sm.add_from_text("bbb", "b.hh");

    auto const labels = cc::vector<label>{at(0, 3, "here", 0), at(0, 3, "and here", 1)};
    auto const out = render_snippet(labels, sm);

    CHECK(out == R"( --> a.hh:1:1
  |
1 | aaa
  | ^^^ here
  |
 ::: b.hh:1:1
  |
1 | bbb
  | --- and here
  |
)");
}

TEST("shaped-linter - snippet - no labels renders nothing")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.hh");
    CHECK(render_snippet({}, sm) == "");
}
