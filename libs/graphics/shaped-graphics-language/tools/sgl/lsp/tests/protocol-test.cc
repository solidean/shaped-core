#include "../protocol/documents.hh"
#include "../protocol/framing.hh"
#include "../protocol/text_index.hh"

#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
cc::span<byte const> bytes_of(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}
} // namespace

TEST("lsp framing - a message split at every byte comes out whole, and two at once come out as two")
{
    auto const one = lsp::frame("{\"a\":1}");
    auto const stream = one + lsp::frame("{\"b\":\"\xc3\xa9\"}");
    for (auto split = isize(0); split <= stream.size(); ++split)
    {
        auto reader = lsp::frame_reader();
        auto out = reader.feed(bytes_of(cc::string_view(stream).subview({.offset = 0, .size = split})));
        for (auto& m : reader.feed(bytes_of(cc::string_view(stream).subview(split))))
            out.push_back(cc::move(m));
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "{\"a\":1}");
        CHECK(out[1] == "{\"b\":\"\xc3\xa9\"}");
        CHECK(reader.pending_size() == 0);
    }
}

TEST("lsp framing - the header name is case-insensitive, other headers are skipped, and no length breaks the stream")
{
    auto reader = lsp::frame_reader();
    auto const out = reader.feed(bytes_of("content-length: 2\r\nContent-Type: application/vscode-jsonrpc; "
                                          "charset=utf-8\r\n\r\n{}"));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "{}");

    nx::expect_error("names no valid Content-Length", nx::exactly(1));
    auto broken = lsp::frame_reader();
    CHECK(broken.feed(bytes_of("Content-Type: x\r\n\r\n{}")).empty());
    CHECK(broken.is_broken());
    CHECK(broken.feed(bytes_of(lsp::frame("{}"))).empty());
}

TEST("lsp text index - positions count UTF-16 units or bytes, and lines end at \\n, \\r\\n and a bare \\r")
{
    // `·` is two bytes and one unit, the emoji four bytes and two units
    auto const text = cc::string_view("a\xc2\xb7"
                                      "b\r\n\xf0\x9f\x98\x80x\rlast");
    auto const index = lsp::text_index(text);
    CHECK(index.line_count() == 3);

    auto const pos = [&](isize offset, lsp::position_encoding e) { return index.position_of(text, offset, e); };
    using lsp::position_encoding;
    auto const at = [](i32 line, i32 character) { return lsp::position{.line = line, .character = character}; };
    CHECK(pos(3, position_encoding::utf16) == at(0, 2));
    CHECK(pos(3, position_encoding::utf8) == at(0, 3));
    CHECK(pos(10, position_encoding::utf16) == at(1, 2)); // after the emoji
    CHECK(pos(10, position_encoding::utf8) == at(1, 4));
    CHECK(pos(12, position_encoding::utf16) == at(2, 0)); // the bare \r ended line 1

    for (auto offset = isize(0); offset <= text.size(); ++offset)
        for (auto const e : {position_encoding::utf8, position_encoding::utf16})
        {
            // inside a sequence a UTF-16 position rounds to the character's end; everything else maps back exactly
            auto const back = index.offset_of(text, pos(offset, e), e);
            auto const is_continuation
                = offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80;
            // the \n of a \r\n has no position of its own: the line ends before its terminator
            auto const is_crlf_tail
                = offset > 0 && offset < text.size() && text[offset] == '\n' && text[offset - 1] == '\r';
            if (!is_crlf_tail && (e == position_encoding::utf8 || !is_continuation))
                CHECK(back == offset);
        }

    // past the end of a line is its end, past the last line is the end of the text
    CHECK(index.offset_of(text, at(0, 99), position_encoding::utf16) == 4);
    CHECK(index.offset_of(text, at(9, 0), position_encoding::utf16) == text.size());
}

TEST("lsp documents - incremental edits apply in order, each against the text the one before left")
{
    auto ws = lsp::workspace();
    ws.open({.uri = "file:///a.sgl", .language_id = "sgl", .version = 1, .text = "let x = 1\nlet y = 2\n"});
    auto const before = ws.snapshot();

    auto const changed = ws.change(
        {
            .uri = "file:///a.sgl",
            .version = 2,
            .content_changes
            = {{.range = lsp::range{.start = {.line = 0, .character = 8}, .end = {.line = 0, .character = 9}},
                .text = "10"},
               {.range = lsp::range{.start = {.line = 1, .character = 4}, .end = {.line = 1, .character = 5}},
                .text = "why"}},
        },
        lsp::position_encoding::utf16);
    CHECK(changed);
    CHECK(ws.find("file:///a.sgl")->text == "let x = 10\nlet why = 2\n");
    CHECK(ws.find("file:///a.sgl")->version == 2);

    // a snapshot taken before still reads what it saw
    CHECK(before.find("file:///a.sgl")->text == "let x = 1\nlet y = 2\n");
    CHECK(before.version_of("file:///a.sgl") == 1);

    ws.close("file:///a.sgl");
    nx::expect_warning("which is not open", nx::exactly(1));
    CHECK(ws.find("file:///a.sgl") == nullptr);
    auto const after_close = ws.change({.uri = "file:///a.sgl", .version = 3}, lsp::position_encoding::utf16);
    CHECK(!after_close);
}

TEST("lsp documents - a file uri and a path convert both ways, drive letters and escapes included")
{
    CHECK(lsp::path_of_uri("file:///c%3A/Projects/a%20b.sgl").value() == "c:/Projects/a b.sgl");
    CHECK(lsp::path_of_uri("file:///home/me/x.sgl").value() == "/home/me/x.sgl");
    CHECK(!lsp::path_of_uri("untitled:Untitled-1").has_value());
    CHECK(lsp::uri_of_path("C:\\Projects\\a b.sgl") == "file:///c%3A/Projects/a%20b.sgl");
    CHECK(lsp::uri_of_path("/home/me/x.sgl") == "file:///home/me/x.sgl");
}
