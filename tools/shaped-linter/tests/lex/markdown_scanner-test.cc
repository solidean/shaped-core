#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/markdown_scanner.hh>
#include <shaped-linter/lex/source_buffer.hh>

using namespace scl;

namespace
{
/// The line kinds of `s`, as a string of one character per line — `t`ext, `b`lank, `f`ence, `c`ode,
/// `|` table.
/// Compact enough that a whole document's shape is one CHECK.
cc::string shape_of(cc::string_view s)
{
    auto const buf = source_buffer::from_text(cc::string(s), "x.md", 0);

    cc::string out;
    for (auto const& line : scan_markdown(buf))
        switch (line.kind)
        {
        case markdown_line_kind::text:
            out += 't';
            break;
        case markdown_line_kind::blank:
            out += 'b';
            break;
        case markdown_line_kind::fence:
            out += 'f';
            break;
        case markdown_line_kind::code:
            out += 'c';
            break;
        case markdown_line_kind::table:
            out += '|';
            break;
        }
    return out;
}
} // namespace

TEST("shaped-linter - markdown - prose and code are told apart")
{
    SECTION("a fenced block is code between two fences")
    {
        CHECK(shape_of("hi\n\n```cpp\nint x;\n```\nbye\n") == "tbfcftb");
    }
    SECTION("a tilde fence works the same")
    {
        CHECK(shape_of("hi\n~~~\ncode\n~~~\n") == "tfcfb");
    }
    SECTION("a heading and a list item are both prose")
    {
        CHECK(shape_of("# title\n\n- one\n- two\n") == "tbttb");
    }
    SECTION("a pipe table is not prose")
    {
        CHECK(shape_of("text\n\n| a | b |\n|---|---|\n| 1 | 2 |\n") == "tb|||b");
    }
}

TEST("shaped-linter - markdown - fence closing rules")
{
    SECTION("the closer must be the same character")
    {
        // The ``` inside a ~~~ block is content, which is how a markdown corpus case is written.
        CHECK(shape_of("~~~\n```\ncode\n```\n~~~\n") == "fcccfb");
    }
    SECTION("a longer closer still closes")
    {
        CHECK(shape_of("```\ncode\n`````\ntext\n") == "fcftb");
    }
    SECTION("a shorter run inside does not close")
    {
        CHECK(shape_of("````\n```\ncode\n````\ntext\n") == "fccftb");
    }
    SECTION("an unterminated fence runs to the end of the file")
    {
        // Including the empty last line, which is inside the block rather than a paragraph break.
        CHECK(shape_of("text\n```\ncode\nmore\n") == "tfccc");
    }
    SECTION("a fence indented more than three columns is not a fence")
    {
        CHECK(shape_of("     ```\n") == "tb");
    }
}
