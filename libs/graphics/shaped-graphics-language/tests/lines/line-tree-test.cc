#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/lines/line_tree.hh>

using namespace cc::primitive_defines;

TEST("sgl line tree - a line is a child of the nearest less-indented line above it")
{
    auto const file = sgl::build_line_tree("A\n"
                                           "B\n"
                                           "    C\n"
                                           "        D\n"
                                           "    E\n"
                                           "  F\n"
                                           "G\n");

    // F lines up with nothing, and is still deeper than B.
    CHECK(sgl::dump_lines(file)
          == "code: A\n"
             "code: B\n"
             "  code: C\n"
             "    code: D\n"
             "  code: E\n"
             "  code: F\n"
             "code: G\n");
}

TEST("sgl line tree - an indented first line is still top level")
{
    auto const file = sgl::build_line_tree("    A\n  B\nC\n");

    CHECK(sgl::dump_lines(file) == "code: A\ncode: B\ncode: C\n");
}

TEST("sgl line tree - a blank line takes the parent of the next non-blank line")
{
    auto const file = sgl::build_line_tree("A\n"
                                           "    B\n"
                                           "\n"
                                           "    C\n"
                                           "   \n"
                                           "D\n"
                                           "\n");

    // Between two children it stays inside; after the last child it sits outside; at the end it is top level.
    // Whitespace on a blank line never makes it a child of anything.
    CHECK(sgl::dump_lines(file)
          == "code: A\n"
             "  code: B\n"
             "  blank\n"
             "  code: C\n"
             "blank\n"
             "code: D\n"
             "blank\n");
}

TEST("sgl line tree - a tab in indentation advances to the next multiple of four and is reported once per line")
{
    auto const file = sgl::build_line_tree("A\n"
                                           "\tB\n"
                                           "  \tC\n"
                                           "    D\n"
                                           "\t\tE\n");

    CHECK(sgl::dump_lines(file)
          == "code: A\n"
             "  code: B\n"
             "  code: C\n"
             "  code: D\n"
             "    code: E\n");
    CHECK(sgl::dump_diagnostics(file)
          == "tab-in-indentation @2+1\n"
             "tab-in-indentation @7+1\n"
             "tab-in-indentation @16+1\n");
}

TEST("sgl line tree - every line ending is kept, and the source prints back byte for byte")
{
    for (auto const source : {
             cc::string_view(""),
             cc::string_view("\n"),
             cc::string_view("no terminator"),
             cc::string_view("unix\n  child\n"),
             cc::string_view("windows\r\n  child\r\n"),
             cc::string_view("old mac\r  child\r"),
             cc::string_view("mixed\r\n\n\r  x\n\r\n"),
             cc::string_view("\xEF\xBB\xBF"
                             "bom\n"),
         })
    {
        auto const file = sgl::build_line_tree(source);
        CHECK(sgl::print_source(file) == source);
    }

    auto const file = sgl::build_line_tree("a\r\nb\rc\nd");
    REQUIRE(file.lines.size() == 4);
    CHECK(file.lines[0].terminator_length == 2);
    CHECK(file.lines[1].terminator_length == 1);
    CHECK(file.lines[2].terminator_length == 1);
    CHECK(file.lines[3].terminator_length == 0);
}
