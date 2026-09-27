#include "../protocol/documents.hh"
#include "../protocol/framing.hh"
#include "../protocol/json.hh"
#include "../protocol/text_index.hh"
#include "../protocol/types.hh"

#include <babel-data/data/json.hh>
#include <clean-core/common/macros.hh>
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

TEST("lsp framing - messages framed before a broken header still come out of the same feed")
{
    nx::expect_error("names no valid Content-Length", nx::exactly(1));
    auto reader = lsp::frame_reader();
    auto const out = reader.feed(bytes_of(lsp::frame("{\"a\":1}") + "Content-Type: x\r\n\r\n{}"));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "{\"a\":1}");
    CHECK(reader.is_broken());
}

TEST("lsp framing - a Content-Length over 64 MiB or a header block over 8 KiB breaks the stream")
{
    nx::expect_error("over the limit", nx::exactly(1));
    auto big_body = lsp::frame_reader();
    CHECK(big_body.feed(bytes_of("Content-Length: 67108865\r\n\r\n")).empty());
    CHECK(big_body.is_broken());

    // exactly 64 MiB is still a length, whose body is merely incomplete
    auto at_limit = lsp::frame_reader();
    CHECK(at_limit.feed(bytes_of("Content-Length: 67108864\r\n\r\n{")).empty());
    CHECK(!at_limit.is_broken());

    // a header that never ends is refused once it is too long, not held forever
    nx::expect_error("header is longer than", nx::exactly(1));
    auto endless = lsp::frame_reader();
    auto const line = cc::string("X-Filler: ") + cc::string::create_filled(100, 'x') + "\r\n";
    auto fed = isize(0);
    while (!endless.is_broken())
    {
        CHECK(endless.feed(bytes_of(line)).empty());
        fed += line.size();
        REQUIRE(fed <= 9 * 1024);
    }
    CHECK(endless.pending_size() == 0);
}

TEST("lsp json - a number reads as an integer only when it is one, within the target's range")
{
    auto const doc = babel::json::read(
        R"([7, -2147483648, 2147483647, 1.5, 2147483648, -2147483649, 1e300, "7", 9007199254740992])");
    REQUIRE(doc.has_value());
    auto const at = [&](isize i) { return doc.value().root()[i]; };

    auto v = i32(-1);
    CHECK(lsp::json::read(at(0), v));
    CHECK(v == 7);
    CHECK(lsp::json::read(at(1), v));
    CHECK(v == -2147483647 - 1);
    CHECK(lsp::json::read(at(2), v));
    CHECK(v == 2147483647);

    v = 42;
    for (auto const i : {3, 4, 5, 6, 7})
        CHECK(!lsp::json::read(at(i), v));
    CHECK(v == 42); // a failed read leaves the target alone

    auto w = i64(0);
    CHECK(lsp::json::read(at(4), w));
    CHECK(w == 2147483648);
    CHECK(lsp::json::read(at(8), w));
    CHECK(w == 9007199254740992);
    CHECK(!lsp::json::read(at(6), w));
    CHECK(!lsp::json::read(at(3), w));
}

TEST("lsp types - a content change with a null range is a whole-text change, and a fractional version does not read")
{
    auto const doc = babel::json::read(
        R"({"null":{"range":null,"text":"x"},"bad":{"range":{"start":{"line":0.5,"character":0},"end":{"line":0,"character":0}},"text":"x"},"open":{"textDocument":{"uri":"file:///a","languageId":"sgl","version":1.5,"text":""}}})");
    REQUIRE(doc.has_value());
    auto const root = doc.value().root();

    auto change = lsp::text_document_content_change{.range = lsp::range{}};
    CHECK(lsp::read(root["null"], change));
    CHECK(!change.range.has_value());
    CHECK(change.text == "x");

    CHECK(!lsp::read(root["bad"], change));
    auto open = lsp::did_open_params();
    CHECK(!lsp::read(root["open"], open));
}

TEST("lsp text index - a default index is the empty text's, which has one line")
{
    auto const index = lsp::text_index();
    CHECK(index.line_count() == 1);
    auto const origin = lsp::position{.line = 0, .character = 0};
    auto const past_end = lsp::position{.line = 0, .character = 5};
    CHECK(index.position_of("", 0, lsp::position_encoding::utf16) == origin);
    CHECK(index.offset_of("", past_end, lsp::position_encoding::utf16) == 0);
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
#ifdef CC_OS_WINDOWS
    CHECK(lsp::path_of_uri("file:///c%3A/Projects/a%20b.sgl").value() == "c:/Projects/a b.sgl");
#else
    CHECK(lsp::path_of_uri("file:///c%3A/Projects/a%20b.sgl").value() == "/c:/Projects/a b.sgl");
#endif
    CHECK(lsp::path_of_uri("file:///home/me/x.sgl").value() == "/home/me/x.sgl");
    CHECK(!lsp::path_of_uri("untitled:Untitled-1").has_value());
    CHECK(lsp::uri_of_path("C:\\Projects\\a b.sgl") == "file:///c%3A/Projects/a%20b.sgl");
    CHECK(lsp::uri_of_path("/home/me/x.sgl") == "file:///home/me/x.sgl");
}

TEST("lsp documents - a file uri's authority, query, fragment and escapes, and what is a drive")
{
    // the query and fragment are no part of the path, and an escaped `?` or `#` is
    CHECK(lsp::path_of_uri("file:///home/a.sgl?x=1#frag").value() == "/home/a.sgl");
    CHECK(lsp::path_of_uri("file:///home/a%3Fb%23c%25d.sgl").value() == "/home/a?b#c%d.sgl");
    CHECK(lsp::path_of_uri("FILE:///home/a.sgl").value() == "/home/a.sgl");
    CHECK(!lsp::path_of_uri("file:///home/bad%zz.sgl").has_value());

    // `localhost` is this machine, in any case
    CHECK(lsp::path_of_uri("file://localhost/home/a.sgl").value() == "/home/a.sgl");
    CHECK(lsp::path_of_uri("file://LocalHost/home/a.sgl").value() == "/home/a.sgl");

    // a first segment that is more than a letter and a colon is never a drive
    CHECK(lsp::path_of_uri("file:///ab:/c").value() == "/ab:/c");
    CHECK(lsp::path_of_uri("file:///a:b/c").value() == "/a:b/c");
#ifdef CC_OS_WINDOWS
    CHECK(lsp::path_of_uri("file://server/share/x%20y.sgl").value() == "\\\\server\\share\\x y.sgl");
    CHECK(lsp::path_of_uri("file:///a:/b").value() == "a:/b");
    CHECK(lsp::path_of_uri("file:///C:").value() == "C:");
#else
    CHECK(lsp::path_of_uri("file://server/share/x%20y.sgl").value() == "//server/share/x y.sgl");
    CHECK(lsp::path_of_uri("file:///a:/b").value() == "/a:/b");
#endif
}
