#include <babel-serializer/data/markdown.hh>
#include <clean-core/common/utility.hh> // cc::min
#include <clean-core/container/span.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>


using namespace cc::primitive_defines;

namespace
{
namespace md = babel::markdown;

// A non-seekable, chunked read source: serves the input in fixed-size chunks through a tiny buffer, so the
// parser must refill mid-line.
// Exercises the streaming path, with lines split across windows.
class chunked_reader
{
public:
    chunked_reader(cc::string_view data, isize chunk)
      : _data(reinterpret_cast<byte const*>(data.data()), data.size()), _chunk(chunk)
    {
    }
    chunked_reader(chunked_reader&&) = delete;
    chunked_reader& operator=(chunked_reader&&) = delete;

    [[nodiscard]] cc::read_stream stream() { return cc::read_stream(_buffer, _buffer, &impl_flush, this); }

private:
    static cc::result<i64>
    impl_flush(byte*& curr, byte*& end, byte*& /*write_end*/, void* ctx, i64 /*off*/, cc::seek_dir /*dir*/, byte* /*fw*/)
    {
        auto& self = *static_cast<chunked_reader*>(ctx);
        auto* const base = self._buffer;
        auto const leftover = isize(end - curr);
        cc::memmove(base, curr, size_t(leftover));

        auto const room = isize(sizeof(self._buffer)) - leftover;
        auto const want = cc::min(self._chunk, room);
        auto const avail = self._data.size() - self._pos;
        auto const n = cc::min(want, avail);
        if (n > 0)
            cc::memcpy(base + leftover, self._data.data() + self._pos, size_t(n));
        self._pos += n;

        curr = base;
        end = base + leftover + n;
        return i64(-1); // no meaningful position
    }

    cc::span<byte const> _data;
    isize _chunk;
    isize _pos = 0;
    byte _buffer[8];
};

/// The first block of a kind, in document order; an invalid ref when there is none.
md::ref first_of(md::document const& doc, md::node_kind kind)
{
    for (auto i = 0; i < doc.node_count(); ++i)
        if (doc.node_at(i).kind() == kind)
            return doc.node_at(i);
    return md::ref();
}
} // namespace

TEST("markdown - empty input is a lone document node")
{
    auto const doc = md::read("").value();
    CHECK(doc.node_count() == 1);
    CHECK(doc.root().is_document());
    CHECK(doc.root().size() == 0);
}

TEST("markdown - ATX headings and their levels")
{
    auto const doc = md::read("# one\n### three\n").value();
    REQUIRE(doc.root().size() == 2);

    CHECK(doc.root()[0].is_heading());
    CHECK(doc.root()[0].level() == 1);
    CHECK(doc.root()[0].text() == "one");

    CHECK(doc.root()[1].level() == 3);
    CHECK(doc.root()[1].text() == "three");
}

TEST("markdown - heading edge cases")
{
    // a closing run of #'s is stripped only when a blank separates it
    CHECK(md::read("## title ##\n").value().root()[0].text() == "title");
    CHECK(md::read("## title##\n").value().root()[0].text() == "title##");

    // seven #'s is not a heading, and `#tag` needs the blank
    CHECK(md::read("####### deep\n").value().root()[0].is_paragraph());
    CHECK(md::read("#tag\n").value().root()[0].is_paragraph());
}

TEST("markdown - fenced code keeps its info string and body verbatim")
{
    auto const doc = md::read("```cpp\nint x{0};\n\nreturn x;\n```\n").value();
    REQUIRE(doc.root().size() == 1);

    auto const code = doc.root()[0];
    REQUIRE(code.is_code_block());
    CHECK(code.info() == "cpp");
    CHECK(code.text() == "int x{0};\n\nreturn x;\n"); // the blank line inside the fence survives
}

TEST("markdown - fence info string is the whole rest of the line")
{
    auto const doc = md::read("```cpp [a-rule] fix=\" = {}\"\ncode\n```\n").value();
    CHECK(doc.root()[0].info() == "cpp [a-rule] fix=\" = {}\"");
    CHECK(doc.root()[0].text() == "code\n");
}

TEST("markdown - bare fence has an empty info string")
{
    auto const doc = md::read("```\nplain\n```\n").value();
    CHECK(doc.root()[0].is_code_block());
    CHECK(doc.root()[0].info() == "");
}

TEST("markdown - tilde fence may contain backticks")
{
    auto const doc = md::read("~~~\n```\nstill code\n~~~\n").value();
    REQUIRE(doc.root().size() == 1);
    CHECK(doc.root()[0].text() == "```\nstill code\n");
}

TEST("markdown - an unterminated fence runs to end of input")
{
    auto const doc = md::read("```cpp\nno close\n").value();
    REQUIRE(doc.root()[0].is_code_block());
    CHECK(doc.root()[0].text() == "no close\n");
}

TEST("markdown - paragraphs join their lines and split on a blank")
{
    auto const doc = md::read("hello\nworld\n\nsecond\n").value();
    REQUIRE(doc.root().size() == 2);
    CHECK(doc.root()[0].text() == "hello\nworld");
    CHECK(doc.root()[1].text() == "second");
}

TEST("markdown - inline spans are left as raw text")
{
    auto const doc = md::read("a **bold** [link](x) `code`\n").value();
    CHECK(doc.root()[0].text() == "a **bold** [link](x) `code`");
    CHECK(doc.root().size() == 1); // no inline children
}

TEST("markdown - thematic breaks")
{
    auto const doc = md::read("---\n***\n___\n").value();
    REQUIRE(doc.root().size() == 3);
    for (auto i = 0; i < 3; ++i)
        CHECK(doc.root()[i].is_thematic_break());

    // two dashes is not enough, and a break wins over a bullet
    CHECK(md::read("--\n").value().root()[0].is_paragraph());
    CHECK(md::read("- item\n").value().root()[0].is_list());
}

TEST("markdown - bullet list items each hold a paragraph")
{
    auto const doc = md::read("- a\n- b\n").value();
    REQUIRE(doc.root().size() == 1);

    auto const list = doc.root()[0];
    REQUIRE(list.is_list());
    CHECK(!list.is_ordered());
    REQUIRE(list.size() == 2);

    CHECK(list[0].is_list_item());
    REQUIRE(list[0].size() == 1);
    CHECK(list[0][0].text() == "a");
    CHECK(list[1][0].text() == "b");
}

TEST("markdown - ordered lists are marked ordered")
{
    auto const doc = md::read("1. first\n2. second\n").value();
    auto const list = doc.root()[0];
    REQUIRE(list.is_list());
    CHECK(list.is_ordered());
    CHECK(list.size() == 2);
}

TEST("markdown - a different marker starts a new list")
{
    auto const doc = md::read("- a\n+ b\n").value();
    REQUIRE(doc.root().size() == 2);
    CHECK(doc.root()[0].is_list());
    CHECK(doc.root()[1].is_list());
}

TEST("markdown - nested list under an item")
{
    auto const doc = md::read("- a\n  - b\n").value();

    auto const outer = doc.root()[0];
    REQUIRE(outer.is_list());
    REQUIRE(outer.size() == 1);

    auto const item = outer[0];
    REQUIRE(item.size() == 2); // its paragraph, then the nested list
    CHECK(item[0].text() == "a");
    REQUIRE(item[1].is_list());
    CHECK(item[1][0][0].text() == "b");
}

TEST("markdown - a list ends when unindented text follows")
{
    auto const doc = md::read("- a\n\nafter\n").value();
    REQUIRE(doc.root().size() == 2);
    CHECK(doc.root()[0].is_list());
    CHECK(doc.root()[1].is_paragraph()); // NOT inside the list
    CHECK(doc.root()[1].text() == "after");
}

TEST("markdown - block quotes nest their content")
{
    auto const doc = md::read("> quoted\n> - a\n").value();

    auto const quote = doc.root()[0];
    REQUIRE(quote.is_block_quote());
    REQUIRE(quote.size() == 2);
    CHECK(quote[0].text() == "quoted");
    REQUIRE(quote[1].is_list());
    CHECK(quote[1][0][0].text() == "a");
}

TEST("markdown - a fence inside a block quote keeps its prefix off the code")
{
    auto const doc = md::read("> ```cpp\n> int x = 0;\n> ```\n").value();

    auto const code = first_of(doc, md::node_kind::code_block);
    REQUIRE(code.is_code_block());
    CHECK(code.info() == "cpp");
    CHECK(code.text() == "int x = 0;\n");
}

TEST("markdown - blocks carry their 1-based source line")
{
    auto const doc = md::read("# one\n\npara\n\n```\ncode\n```\n").value();
    CHECK(doc.root()[0].line() == 1); // heading
    CHECK(doc.root()[1].line() == 3); // paragraph
    CHECK(doc.root()[2].line() == 5); // the opening fence
}

TEST("markdown - CRLF input parses like LF")
{
    auto const doc = md::read("# one\r\n\r\nbody\r\n").value();
    REQUIRE(doc.root().size() == 2);
    CHECK(doc.root()[0].text() == "one");
    CHECK(doc.root()[1].text() == "body");
}

TEST("markdown - kind-tolerant accessors and invalid refs")
{
    auto const doc = md::read("# title\n").value();
    auto const root = doc.root();

    CHECK(root.text() == "");   // a container carries no text
    CHECK(root.info() == "");   // not a code block
    CHECK(root.level() == 0);   // not a heading
    CHECK(!root.is_ordered());  // not a list
    CHECK(!root[7].is_valid()); // out of range
    CHECK(!doc.node_at(99).is_valid());
    CHECK(!doc.node_at(-1).is_valid());
    CHECK(md::ref().line() == 0); // a default ref answers, it does not fault
    CHECK(md::ref().text() == "");
    CHECK(md::ref().size() == 0);
}

TEST("markdown - preorder iteration visits every block")
{
    auto const doc = md::read("# h\n\n- a\n- b\n").value();

    // document, heading, list, item, paragraph, item, paragraph
    REQUIRE(doc.node_count() == 7);
    CHECK(doc.node_at(0).is_document());
    CHECK(doc.node_at(1).is_heading());
    CHECK(doc.node_at(2).is_list());
    CHECK(doc.node_at(3).is_list_item());
    CHECK(doc.node_at(4).is_paragraph());
    CHECK(doc.node_at(5).is_list_item());
    CHECK(doc.node_at(6).is_paragraph());
}

TEST("markdown - parsing over a chunked stream matches in-memory")
{
    auto const text = cc::string_view("# title\n\nsome text\n\n```cpp x\nint v{};\n```\n\n- a\n- b\n");
    auto const want = md::read(text).value();

    for (auto const chunk : {isize(1), isize(3), isize(7)})
    {
        chunked_reader reader = {text, chunk};
        auto stream = reader.stream();
        auto const doc = md::read(stream).value();

        REQUIRE(doc.node_count() == want.node_count());
        for (auto i = 0; i < doc.node_count(); ++i)
        {
            CHECK(doc.node_at(i).kind() == want.node_at(i).kind());
            CHECK(doc.node_at(i).text() == want.node_at(i).text());
            CHECK(doc.node_at(i).info() == want.node_at(i).info());
            CHECK(doc.node_at(i).line() == want.node_at(i).line());
        }
    }
}
