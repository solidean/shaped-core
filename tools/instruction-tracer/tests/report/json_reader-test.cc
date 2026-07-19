#include <instruction-tracer/report/json_reader.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
json_value parse_ok(cc::string_view text)
{
    auto r = parse_json(text);
    REQUIRE(r.has_value());
    return cc::move(r).value();
}
} // namespace

TEST("json_reader - scalars")
{
    CHECK(parse_ok("null").is_null());
    CHECK(parse_ok("true").as_bool() == true);
    CHECK(parse_ok("false").as_bool() == false);
    CHECK(parse_ok("42").as_number() == 42.0);
    CHECK(parse_ok("-7").as_number() == -7.0);
    CHECK(parse_ok("3.5").as_number() == 3.5);
    CHECK(parse_ok("1e3").as_number() == 1000.0);
    CHECK(parse_ok("-2.5e-2").as_number() == -0.025);
    CHECK(parse_ok("\"hello\"").as_string() == "hello");
}

TEST("json_reader - leading and trailing whitespace")
{
    CHECK(parse_ok("  \n\t 123 \r\n ").as_number() == 123.0);
    auto o = parse_ok("\n{ \"a\" : 1 }\n");
    REQUIRE(o.is_object());
    REQUIRE(o.find("a") != nullptr);
    CHECK(o.find("a")->as_number() == 1.0);
}

TEST("json_reader - array")
{
    auto a = parse_ok("[1, 2, 3, \"x\", true, null]");
    REQUIRE(a.is_array());
    REQUIRE(a.size() == 6);
    CHECK(a.at(0)->as_number() == 1.0);
    CHECK(a.at(2)->as_number() == 3.0);
    CHECK(a.at(3)->as_string() == "x");
    CHECK(a.at(4)->as_bool() == true);
    CHECK(a.at(5)->is_null());
    CHECK(a.at(6) == nullptr);
}

TEST("json_reader - empty containers")
{
    CHECK(parse_ok("[]").is_array());
    CHECK(parse_ok("[]").size() == 0);
    CHECK(parse_ok("{}").is_object());
    CHECK(parse_ok("{}").size() == 0);
    CHECK(parse_ok("\"\"").as_string() == "");
}

TEST("json_reader - nested object")
{
    auto o = parse_ok(R"({"outer": {"inner": [10, 20], "name": "n"}, "flag": false})");
    REQUIRE(o.is_object());
    auto const* outer = o.find("outer");
    REQUIRE(outer != nullptr);
    REQUIRE(outer->is_object());
    auto const* inner = outer->find("inner");
    REQUIRE(inner != nullptr);
    REQUIRE(inner->is_array());
    CHECK(inner->at(1)->as_number() == 20.0);
    CHECK(outer->find("name")->as_string() == "n");
    CHECK(o.find("flag")->as_bool() == false);
    CHECK(o.find("missing") == nullptr);
}

TEST("json_reader - string escapes")
{
    CHECK(parse_ok(R"("a\"b")").as_string() == "a\"b");
    CHECK(parse_ok(R"("a\\b")").as_string() == "a\\b");
    CHECK(parse_ok(R"("a\/b")").as_string() == "a/b");
    CHECK(parse_ok(R"("tab\tend")").as_string() == "tab\tend");
    CHECK(parse_ok(R"("nl\nend")").as_string() == "nl\nend");
}

TEST("json_reader - unicode escapes")
{
    // json_writer emits '<' as < so "</script>" cannot break out; the reader turns it back.
    CHECK(parse_ok("\"\\u003c/script>\"").as_string() == "</script>");
    // BMP multi-byte: U+00E9 -> 2 UTF-8 bytes (0xC3 0xA9).
    CHECK(parse_ok("\"caf\\u00e9\"").as_string() == "caf\xc3\xa9");
    // Surrogate pair: U+1F600 = D83D DE00 -> 4 UTF-8 bytes (0xF0 0x9F 0x98 0x80).
    CHECK(parse_ok("\"\\ud83d\\ude00\"").as_string() == "\xf0\x9f\x98\x80");
}

TEST("json_reader - kind-tolerant accessors return fallbacks")
{
    auto o = parse_ok(R"({"n": 5})");
    // asking for the wrong type yields the fallback, never a crash.
    CHECK(o.find("n")->as_string("fb") == "fb");
    CHECK(o.as_number(-1) == -1.0);
    CHECK(o.find("absent") == nullptr);
}

TEST("json_reader - malformed input is a loud error, not a crash")
{
    CHECK(parse_json("").has_error());
    CHECK(parse_json("{").has_error());
    CHECK(parse_json("[1, 2").has_error());
    CHECK(parse_json("{\"a\": }").has_error());
    CHECK(parse_json("{\"a\" 1}").has_error());
    CHECK(parse_json("nul").has_error());
    CHECK(parse_json("\"unterminated").has_error());
    CHECK(parse_json("[1, 2] extra").has_error());
    CHECK(parse_json("1.2.3").has_error());
    CHECK(parse_json("\"\\q\"").has_error()); // invalid escape
}
