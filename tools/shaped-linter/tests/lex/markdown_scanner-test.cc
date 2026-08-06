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
/// `|` table, `y` frontmatter.
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
        case markdown_line_kind::frontmatter:
            out += 'y';
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

TEST("shaped-linter - markdown - frontmatter only where it is unambiguous")
{
    SECTION("a leading block is frontmatter, delimiters included")
    {
        CHECK(shape_of("---\nname: x\n---\n\nbody\n") == "yyybtb");
    }
    SECTION("a blank line inside it does not end it")
    {
        CHECK(shape_of("---\nname: x\n\ndescription: y\n---\nbody\n") == "yyyyytb");
    }
    SECTION("without a closer the opener is a thematic break")
    {
        CHECK(shape_of("---\nname: x\n\nbody\n") == "ttbtb");
    }
    SECTION("a rule below line one is never an opener")
    {
        CHECK(shape_of("intro\n\n---\n\nbody\n") == "tbtbtb");
    }
    SECTION("a --- inside a fenced block stays code")
    {
        CHECK(shape_of("```\n---\n```\n") == "fcfb");
    }
}
