#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/lex/python_lexer.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/prose/prose_view.hh>

using namespace scl;

namespace
{
/// Owns the buffer the view's `string_view`s point into.
struct extracted
{
    cc::unique_ptr<source_buffer> buf;
    token_stream ts;
    prose_view view;

    /// Every prose line, in order, `|`-joined — the whole extraction as one comparable string.
    cc::string lines() const
    {
        cc::string out;
        for (auto const& b : view.blocks)
            for (auto const& l : b.lines)
            {
                if (!out.empty())
                    out += '|';
                out += l.text;
            }
        return out;
    }

    /// One character per block, giving how many lines each holds — the block grouping at a glance.
    cc::string block_sizes() const
    {
        cc::string out;
        for (auto const& b : view.blocks)
            out += char('0' + b.lines.size());
        return out;
    }
};

extracted extract(cc::string_view text, cc::string_view path)
{
    auto buf = cc::make_unique<source_buffer>(source_buffer::from_text(cc::string(text), path, 0));
    auto const language = language_from_path(path);

    token_stream ts;
    if (language == source_language::cpp)
        ts = lex(*buf).value();
    else if (language == source_language::python)
        ts = lex_python(*buf).value();

    auto view = extract_prose(*buf, language, ts);
    return {.buf = cc::move(buf), .ts = cc::move(ts), .view = cc::move(view)};
}
} // namespace

TEST("shaped-linter - prose - C++ comments, markers stripped")
{
    SECTION("a line comment loses its slashes and its leading space")
    {
        CHECK(extract("//  hello\n", "a.cc").lines() == "hello");
    }
    SECTION("a doc comment likewise")
    {
        CHECK(extract("/// hello\n", "a.cc").lines() == "hello");
    }
    SECTION("code is not prose")
    {
        CHECK(extract("int x = 1; // note\nint y = 2;\n", "a.cc").lines() == "note");
    }
    SECTION("a string that looks like a comment stays code")
    {
        CHECK(extract("auto s = \"// not a comment\";\n", "a.cc").lines() == "");
    }
}

TEST("shaped-linter - prose - block comments come apart into their lines")
{
    auto const e = extract("/* one\n * two\n * three */\n", "a.cc");
    CHECK(e.lines() == "one|two|three");
    CHECK(e.block_sizes() == "3");
}

TEST("shaped-linter - prose - adjacent line comments are one block")
{
    SECTION("consecutive lines group")
    {
        auto const e = extract("// one\n// two\n", "a.cc");
        CHECK(e.block_sizes() == "2");
    }
    SECTION("a gap starts a new block")
    {
        auto const e = extract("// one\n\n// two\n", "a.cc");
        CHECK(e.block_sizes() == "11");
    }
    SECTION("a trailing comment after code still continues its run")
    {
        auto const e = extract("// one\nint x; // two\n", "a.cc");
        CHECK(e.block_sizes() == "2");
    }
}

TEST("shaped-linter - prose - Python comments and docstrings")
{
    SECTION("a hash comment loses its marker")
    {
        CHECK(extract("#  hello\n", "a.py").lines() == "hello");
    }
    SECTION("a docstring is prose, line by line")
    {
        auto const e = extract("def f():\n    \"\"\"One.\n    Two.\n    \"\"\"\n", "a.py");
        CHECK(e.lines() == "One.|Two.");
        CHECK(e.block_sizes() == "2");
    }
    SECTION("a triple-quoted string used as data is not a docstring")
    {
        // Only a string that OPENS a logical line is documentation; anything else is a value.
        CHECK(extract("x = \"\"\"data. here\"\"\"\n", "a.py").lines() == "");
    }
    SECTION("an ordinary string is never prose")
    {
        CHECK(extract("x = 'hello. world'\n", "a.py").lines() == "");
    }
}

TEST("shaped-linter - prose - measuring counts extracted text only")
{
    SECTION("markers, leaders and code are outside the count")
    {
        auto const s = measure_prose(extract("/// one two\nint x = 1; // three\n", "a.cc").view);
        CHECK(s.lines == 2);
        CHECK(s.words == 3);
    }
    SECTION("a run of spaces is one separator, not several words")
    {
        CHECK(measure_prose(extract("//   one    two   \n", "a.cc").view).words == 2);
    }
    SECTION("a file with no prose measures zero")
    {
        auto const s = measure_prose(extract("int x = 1;\n", "a.cc").view);
        CHECK(s.lines == 0);
        CHECK(s.words == 0);
    }
    SECTION("markdown counts the markers it keeps")
    {
        // `# Title` extracts marker and all, so it measures as two words — the delta stays comparable
        // because both sides are measured the same way.
        auto const s = measure_prose(extract("# Title\n\nbody text\n", "a.md").view);
        CHECK(s.lines == 2);
        CHECK(s.words == 4);
    }
}

TEST("shaped-linter - prose - markdown body text, never its code")
{
    SECTION("paragraphs are blocks and fences are skipped")
    {
        auto const e = extract("one\ntwo\n\n```cpp\nint x; // c\n```\n\nthree\n", "a.md");
        CHECK(e.lines() == "one|two|three");
        CHECK(e.block_sizes() == "21");
    }
    SECTION("a heading is prose, marker and all")
    {
        // Markdown markers are not stripped: unlike a `//`, they sit inline with the text, and a rule that
        // cares (the ordered-list `1.`) is better off recognizing them than having them silently removed.
        CHECK(extract("# Title\n\nbody\n", "a.md").lines() == "# Title|body");
    }
    SECTION("a table row is not")
    {
        CHECK(extract("text\n\n| a. b | c |\n", "a.md").lines() == "text");
    }
    SECTION("frontmatter is metadata, not prose")
    {
        // A skill file's `description:` is one scalar the harness consumes whole, so no prose rule may
        // ask it to split at a seam.
        auto const e = extract("---\nname: x\ndescription: One sentence. Then another.\n---\n\nbody\n", "a.md");
        CHECK(e.lines() == "body");
    }
}
