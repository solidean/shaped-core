#include <babel-serializer/data/json.hh>
#include <clean-core/common/utility.hh> // cc::min
#include <clean-core/container/span.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

#include <cstring> // std::memmove, std::memcpy

namespace
{
// A non-seekable, chunked read source: serves the input in fixed-size chunks through a tiny buffer, so the
// parser must refill mid-value. Exercises the streaming path (strings / tokens split across windows).
class chunked_reader
{
public:
    chunked_reader(cc::string_view data, cc::isize chunk)
      : _data(reinterpret_cast<cc::byte const*>(data.data()), data.size()), _chunk(chunk)
    {
    }
    chunked_reader(chunked_reader&&) = delete;
    chunked_reader& operator=(chunked_reader&&) = delete;

    [[nodiscard]] cc::read_stream stream() { return cc::read_stream(_buffer, _buffer, &impl_flush, this); }

private:
    static cc::result<cc::i64> impl_flush(cc::byte*& curr,
                                          cc::byte*& end,
                                          cc::byte*& /*write_end*/,
                                          void* ctx,
                                          cc::i64 /*off*/,
                                          cc::seek_dir /*dir*/,
                                          cc::byte* /*fw*/)
    {
        auto& self = *static_cast<chunked_reader*>(ctx);
        auto* const base = self._buffer;
        auto const leftover = cc::isize(end - curr);
        std::memmove(base, curr, size_t(leftover));

        auto const room = cc::isize(sizeof(self._buffer)) - leftover;
        auto const want = cc::min(self._chunk, room);
        auto const avail = self._data.size() - self._pos;
        auto const n = cc::min(want, avail);
        if (n > 0)
            std::memcpy(base + leftover, self._data.data() + self._pos, size_t(n));
        self._pos += n;

        curr = base;
        end = base + leftover + n;
        return cc::i64(-1); // no meaningful position
    }

    cc::span<cc::byte const> _data;
    cc::isize _chunk;
    cc::isize _pos = 0;
    cc::byte _buffer[8];
};
} // namespace

TEST("json - scalar values")
{
    CHECK(babel::json::read("true").value().root().as_bool() == true);
    CHECK(babel::json::read("false").value().root().as_bool() == false);
    CHECK(babel::json::read("null").value().root().is_null());
    CHECK(babel::json::read("42").value().root().as_double() == 42);
    CHECK(babel::json::read("-1.5e2").value().root().as_double() == -150.0);
    CHECK(babel::json::read("\"hello\"").value().root().as_string() == "hello");
}

TEST("json - nested object and array traversal")
{
    auto const doc = babel::json::read(R"({"a": 1, "b": [10, 20, 30], "c": {"d": true}})").value();
    auto const root = doc.root();

    REQUIRE(root.is_object());
    CHECK(root.size() == 3);
    CHECK(root["a"].as_double() == 1);

    auto const b = root["b"];
    REQUIRE(b.is_array());
    CHECK(b.size() == 3);
    CHECK(b[0].as_double() == 10);
    CHECK(b[1].as_double() == 20);
    CHECK(b[2].as_double() == 30);

    CHECK(root["c"]["d"].as_bool() == true);

    // positional access carries the member key
    CHECK(root[0].key() == "a");
    CHECK(root[2].key() == "c");
    CHECK(root.has("b"));
    CHECK(!root.has("zzz"));
}

TEST("json - string escapes")
{
    // backslash escapes (raw C++ literal: the parser sees literal \n \t \" \\ \/)
    CHECK(babel::json::read(R"("a\n\t\"\\\/b")").value().root().as_string() == "a\n\t\"\\/b");

    // \uXXXX in the BMP, built as an ASCII-only literal ("\\u" is the two chars backslash-u):
    // BMP escapes: decodes to 'A' followed by code point U+00E9 (UTF-8 bytes C3 A9)
    CHECK(babel::json::read("\"\\u0041\\u00e9\"").value().root().as_string() == "A\xC3\xA9");

    // surrogate pair: decodes to code point U+1F600 (UTF-8 bytes F0 9F 98 80)
    CHECK(babel::json::read("\"\\uD83D\\uDE00\"").value().root().as_string() == "\xF0\x9F\x98\x80");
}

TEST("json - empty object and array")
{
    CHECK(babel::json::read("{}").value().root().size() == 0);
    CHECK(babel::json::read("[]").value().root().size() == 0);
    CHECK(babel::json::read("{}").value().root().is_object());
    CHECK(babel::json::read("[]").value().root().is_array());
}

TEST("json - kind-tolerant accessors and invalid refs")
{
    auto const doc = babel::json::read("42").value();
    auto const root = doc.root();

    CHECK(root.as_string("fallback") == "fallback"); // wrong kind -> fallback
    CHECK(root.as_bool(true) == true);
    CHECK(!root["missing"].is_valid()); // subscript on a non-object
    CHECK(!root[0].is_valid());         // subscript on a non-array
    CHECK(root["missing"].as_double(7) == 7);
}

TEST("json - errors")
{
    CHECK(babel::json::read("").has_error());           // empty input
    CHECK(babel::json::read("   ").has_error());        // only whitespace
    CHECK(babel::json::read("{").has_error());          // unterminated object
    CHECK(babel::json::read("[1, 2").has_error());      // unterminated array
    CHECK(babel::json::read("[1 2]").has_error());      // missing comma
    CHECK(babel::json::read("nul").has_error());        // truncated literal
    CHECK(babel::json::read("true false").has_error()); // trailing junk
    CHECK(babel::json::read(R"({"k": })").has_error()); // missing value
}

TEST("json - parsing over a chunked stream matches in-memory")
{
    auto const text = cc::string_view(R"({"msg": "hello world", "nums": [1, 2, 3], "nested": {"ok": true}})");

    for (auto const chunk : {cc::isize(1), cc::isize(2), cc::isize(5)})
    {
        chunked_reader reader{text, chunk};
        auto stream = reader.stream();
        auto const doc = babel::json::read(stream).value();
        auto const root = doc.root();

        CHECK(root["msg"].as_string() == "hello world");
        CHECK(root["nums"].size() == 3);
        CHECK(root["nums"][2].as_double() == 3);
        CHECK(root["nested"]["ok"].as_bool() == true);
    }
}
