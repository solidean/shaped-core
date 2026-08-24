#include <babel-serializer/data/json.hh>
#include <clean-core/common/utility.hh> // cc::min
#include <clean-core/container/span.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>


using namespace cc::primitive_defines;

namespace
{
// A non-seekable, chunked read source: serves the input in fixed-size chunks through a tiny buffer, so the
// parser must refill mid-value.
// Exercises the streaming path, with strings and tokens split across windows.
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

    for (auto const chunk : {isize(1), isize(2), isize(5)})
    {
        auto reader = chunked_reader(text, chunk);
        auto stream = reader.stream();
        auto const doc = babel::json::read(stream).value();
        auto const root = doc.root();

        CHECK(root["msg"].as_string() == "hello world");
        CHECK(root["nums"].size() == 3);
        CHECK(root["nums"][2].as_double() == 3);
        CHECK(root["nested"]["ok"].as_bool() == true);
    }
}

// -------------------------------------------------------------------------------------------------
// writing

namespace
{
/// Runs `build` against a string_writer and returns the text it produced.
template <class F>
cc::string written(babel::json::write_options opts, F&& build)
{
    auto w = babel::json::string_writer(opts);
    build(w);
    return w.finish().value();
}
} // namespace

TEST("json - write scalars and containers")
{
    CHECK(written({}, [](auto& w) { w.write(nullptr); }) == "null");
    CHECK(written({}, [](auto& w) { w.write(true); }) == "true");
    CHECK(written({}, [](auto& w) { w.write(42); }) == "42");
    CHECK(written({}, [](auto& w) { w.write(-7); }) == "-7");
    CHECK(written({}, [](auto& w) { w.write(0.5); }) == "0.5");
    CHECK(written({}, [](auto& w) { w.write("hi"); }) == "\"hi\"");

    CHECK(written({}, [](auto& w) { auto o = w.object(); }) == "{}");
    CHECK(written({}, [](auto& w) { auto a = w.array(); }) == "[]");

    auto const nested = written({},
                                [](auto& w)
                                {
                                    auto o = w.object();
                                    o.write("name", "shaped");
                                    o.write("n", 3);
                                    {
                                        auto tags = o.write_array("tags");
                                        tags.write(1);
                                        tags.write(2);
                                        auto inner = tags.write_object();
                                        inner.write("deep", true);
                                    }
                                    o.write("done", nullptr);
                                });
    CHECK(nested == R"({"name":"shaped","n":3,"tags":[1,2,{"deep":true}],"done":null})");
}

TEST("json - write indented")
{
    auto const text = written({.indent = 2},
                              [](auto& w)
                              {
                                  auto o = w.object();
                                  o.write("a", 1);
                                  auto arr = o.write_array("b");
                                  arr.write(1);
                                  arr.write(2);
                              });

    CHECK(text == "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ]\n}");

    // an empty container stays on one line whatever the indent
    CHECK(written({.indent = 4}, [](auto& w) { auto o = w.object(); }) == "{}");

    auto const four = written({.indent = 4},
                              [](auto& w)
                              {
                                  auto o = w.object();
                                  auto inner = o.write_object("x");
                                  inner.write("y", 1);
                              });
    CHECK(four == "{\n    \"x\": {\n        \"y\": 1\n    }\n}");
}

TEST("json - write escapes")
{
    CHECK(written({}, [](auto& w) { w.write("a\"b\\c"); }) == R"("a\"b\\c")");
    CHECK(written({}, [](auto& w) { w.write("\b\f\n\r\t"); }) == R"("\b\f\n\r\t")");
    CHECK(written({}, [](auto& w) { w.write(cc::string_view("\x01\x1F")); }) == "\"\\u0001\\u001f\"");

    // UTF-8 passes through byte-for-byte by default
    CHECK(written({}, [](auto& w) { w.write("\xC3\xA4"); }) == "\"\xC3\xA4\"");

    // ...and becomes \uXXXX on request, astral code points as a surrogate pair
    CHECK(written({.escape_non_ascii = true}, [](auto& w) { w.write("\xC3\xA4"); }) == "\"\\u00e4\"");
    CHECK(written({.escape_non_ascii = true}, [](auto& w) { w.write("\xF0\x9F\x98\x80"); }) == "\"\\ud83d\\ude00\"");

    // a byte that does not decode is passed on rather than replaced or rejected
    CHECK(written({.escape_non_ascii = true}, [](auto& w) { w.write(cc::string_view("\xFF")); }) == "\"\xFF\"");

    // keys go through the same escaper
    auto const keyed = written({},
                               [](auto& w)
                               {
                                   auto o = w.object();
                                   o.write("a\nb", 1);
                               });
    CHECK(keyed == R"({"a\nb":1})");
}

TEST("json - write floats")
{
    CHECK(written({}, [](auto& w) { w.write(0.1); }) == "0.1"); // shortest round-trip, not 0.100000000000000006
    CHECK(written({}, [](auto& w) { w.write(1.0); }) == "1");
    CHECK(written({.floats = cc::float_notation::fixed, .float_precision = 2}, [](auto& w) { w.write(1.5); }) == "1.50");
    CHECK(written({.floats = cc::float_notation::scientific, .float_precision = 1}, [](auto& w) { w.write(1500.0); })
          == "1.5e+03");

    // one value can pick its own notation without touching the writer's default
    auto const mixed = written({},
                               [](auto& w)
                               {
                                   auto o = w.object();
                                   o.write("shortest", 1.5);
                                   o.write("fixed", 1.5, cc::float_notation::fixed, 3);
                               });
    CHECK(mixed == R"({"shortest":1.5,"fixed":1.500})");
}

TEST("json - write non-finite")
{
    auto const inf = 1e308 * 10;
    auto const nan = inf - inf;

    SECTION("error is the default")
    {
        auto w = babel::json::string_writer();
        w.write(nan);
        CHECK(w.finish().has_error());
    }

    SECTION("null")
    {
        CHECK(written({.non_finite = babel::json::non_finite_policy::null}, [&](auto& w) { w.write(nan); }) == "null");
        CHECK(written({.non_finite = babel::json::non_finite_policy::null}, [&](auto& w) { w.write(inf); }) == "null");
    }

    SECTION("string")
    {
        auto const opts = babel::json::write_options{.non_finite = babel::json::non_finite_policy::string};
        CHECK(written(opts, [&](auto& w) { w.write(nan); }) == "\"NaN\"");
        CHECK(written(opts, [&](auto& w) { w.write(inf); }) == "\"Infinity\"");
        CHECK(written(opts, [&](auto& w) { w.write(-inf); }) == "\"-Infinity\"");
    }
}

TEST("json - write raw and ascii")
{
    auto const text = written({},
                              [](auto& w)
                              {
                                  auto o = w.object();
                                  o.write_raw("pre", R"([1,{"a":2}])");
                                  o.write_ascii("uml", "\xC3\xA4");
                                  auto a = o.write_array("more");
                                  a.write_raw("1e999");
                              });
    CHECK(text == "{\"pre\":[1,{\"a\":2}],\"uml\":\"\\u00e4\",\"more\":[1e999]}");
}

TEST("json - write newline-delimited")
{
    auto const text = written({.indent = 2, .newline_delimited = true}, // nd forces compact whatever the indent says
                              [](auto& w)
                              {
                                  for (auto i = 0; i < 3; ++i)
                                  {
                                      auto o = w.object();
                                      o.write("i", i);
                                  }
                              });
    CHECK(text == "{\"i\":0}\n{\"i\":1}\n{\"i\":2}\n");
}

TEST("json - write round-trips through the reader")
{
    auto const text = written({.indent = 2},
                              [](auto& w)
                              {
                                  auto o = w.object();
                                  o.write("msg", "hello \"world\"\n");
                                  o.write("pi", 3.25);
                                  o.write("big", u64(9007199254740993ull));
                                  o.write("neg", -12345);
                                  o.write("yes", true);
                                  o.write("nothing", nullptr);
                                  {
                                      auto arr = o.write_array("list");
                                      arr.write(1);
                                      arr.write("two");
                                      {
                                          auto sub = arr.write_object();
                                          sub.write("three", 3.5);
                                      }
                                      auto empty = arr.write_array();
                                  }
                              });

    auto const doc = babel::json::read(text).value();
    auto const root = doc.root();
    CHECK(root["msg"].as_string() == "hello \"world\"\n");
    CHECK(root["pi"].as_double() == 3.25);
    CHECK(root["neg"].as_double() == -12345);
    CHECK(root["yes"].as_bool());
    CHECK(root["nothing"].is_null());
    REQUIRE(root["list"].size() == 4);
    CHECK(root["list"][1].as_string() == "two");
    CHECK(root["list"][2]["three"].as_double() == 3.5);
    CHECK(root["list"][3].size() == 0);
}

TEST("json - write errors are sticky")
{
    // a span sink far too small for the document: the first overflowing write fails, the rest are no-ops
    byte buffer[8];
    auto adapter = cc::span_write_stream_adapter(buffer);
    cc::write_stream stream = adapter;
    auto w = babel::json::writer(stream);

    {
        auto o = w.object();
        for (auto i = 0; i < 100; ++i)
            o.write("key", "a long value that will not fit");
    }
    CHECK(w.has_error());
    CHECK(w.finish().has_error());
}

TEST("json - write into a bounded span sink")
{
    // the writer only ever sees a cc::write_stream, so a growing buffer and a bounded span behave the same
    byte buffer[64];
    auto adapter = cc::span_write_stream_adapter(buffer);
    cc::write_stream stream = adapter;

    {
        auto w = babel::json::writer(stream);
        {
            auto o = w.object();
            o.write("ok", true);
        }
        REQUIRE(w.finish().has_value());
    }

    CHECK(cc::string_view(reinterpret_cast<char const*>(buffer), 11) == R"({"ok":true})");
}
