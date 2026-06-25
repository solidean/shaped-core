#include <clean-core/container/span.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

// =========================================================================================================
// Compile-time checks for the grammar parser and the type classifier (run during compilation).
// =========================================================================================================

namespace
{
constexpr cc::impl::format_spec test_parse(cc::string_view s)
{
    return cc::impl::parse_spec(s);
}

consteval cc::isize first_field_index(cc::string_view s)
{
    cc::impl::index_state ix;
    return cc::impl::parse_field(s, 0, ix).arg_index;
}
} // namespace

// spec parsing
static_assert(test_parse("").presentation == '\0');
static_assert(test_parse("d").presentation == 'd');
static_assert(test_parse("06x").width == 6);
static_assert(test_parse("06x").zero_pad);
static_assert(test_parse("06x").presentation == 'x');
static_assert(test_parse(".2f").precision == 2);
static_assert(test_parse(".2f").presentation == 'f');
static_assert(test_parse("*^7").fill == '*');
static_assert(test_parse("*^7").align == cc::impl::align_t::center);
static_assert(test_parse("*^7").width == 7);
static_assert(test_parse("+").sign == cc::impl::sign_t::plus);
static_assert(test_parse(">10").align == cc::impl::align_t::right);
static_assert(test_parse("'").group == '\'');
static_assert(test_parse(",").group == ',');
static_assert(test_parse("8'").width == 8 && test_parse("8'").group == '\'');
static_assert(test_parse("'.2f").group == '\'' && test_parse("'.2f").precision == 2);

// field index resolution
static_assert(first_field_index("{}") == 0);
static_assert(first_field_index("{3}") == 3);
static_assert(first_field_index("{:>5}") == 0);

// type classification
static_assert(cc::impl::type_tag_of<int>() == cc::impl::type_tag::sint);
static_assert(cc::impl::type_tag_of<unsigned>() == cc::impl::type_tag::uint);
static_assert(cc::impl::type_tag_of<double>() == cc::impl::type_tag::floating);
static_assert(cc::impl::type_tag_of<bool>() == cc::impl::type_tag::boolean);
static_assert(cc::impl::type_tag_of<char>() == cc::impl::type_tag::character);
static_assert(cc::impl::type_tag_of<char const*>() == cc::impl::type_tag::string_like);
static_assert(cc::impl::type_tag_of<cc::string>() == cc::impl::type_tag::string_like);

// =========================================================================================================
// Custom types: a cc::custom::formatter that takes no spec, one that delegates the standard spec to its
// inner value, and a type with a member to_string() (plain "{}").
// =========================================================================================================

namespace
{
struct vec2
{
    int x;
    int y;
};

// formats its temperature value with the given (standard) spec, then appends "C"
struct celsius
{
    double value;
};

struct greeting
{
    cc::string to_string() const { return "hi!"; }
};
} // namespace

// vec2 defines its own (empty) spec language and validation
template <>
struct cc::custom::formatter<vec2>
{
    static consteval void validate(cc::string_view spec)
    {
        if (!spec.empty())
            throw "vec2 takes no format spec";
    }
    static void format(cc::format_sink out, cc::string_view /*spec*/, vec2 const& v)
    {
        out.put("(");
        cc::format_value(out, "", v.x); // delegate the components to the standard formatting
        out.put(", ");
        cc::format_value(out, "", v.y);
        out.put(")");
    }
};

// celsius delegates both validation and formatting to the standard grammar
template <>
struct cc::custom::formatter<celsius>
{
    static consteval void validate(cc::string_view spec) { cc::validate_format_spec(spec); }
    static void format(cc::format_sink out, cc::string_view spec, celsius const& c)
    {
        cc::format_value(out, spec, c.value);
        out.put("C");
    }
};

// =========================================================================================================
// Runtime behavior
// =========================================================================================================

TEST("format - literals and escapes")
{
    CHECK(cc::format("") == "");
    CHECK(cc::format("hello") == "hello");
    CHECK(cc::format("{{}}") == "{}");
    CHECK(cc::format("a{{b}}c") == "a{b}c");
    CHECK(cc::format("100{{%}}") == "100{%}");
}

TEST("format - argument indexing")
{
    CHECK(cc::format("{}", 42) == "42");
    CHECK(cc::format("{} {}", 1, 2) == "1 2");
    CHECK(cc::format("{1} {0}", "a", "b") == "b a");
    CHECK(cc::format("{0}{0}{1}", "x", "y") == "xxy");
}

TEST("format - integers")
{
    CHECK(cc::format("{:d}", 42) == "42");
    CHECK(cc::format("{}", -42) == "-42");
    CHECK(cc::format("{:x}", 255) == "ff");
    CHECK(cc::format("{:X}", 255) == "FF");
    CHECK(cc::format("{:#x}", 255) == "0xff");
    CHECK(cc::format("{:#X}", 255) == "0XFF");
    CHECK(cc::format("{:#06x}", 255) == "0x00ff");
    CHECK(cc::format("{:o}", 8) == "10");
    CHECK(cc::format("{:#o}", 8) == "0o10");
    CHECK(cc::format("{:b}", 5) == "101");
    CHECK(cc::format("{:#b}", 5) == "0b101");
}

TEST("format - integer sign and zero-padding")
{
    CHECK(cc::format("{:+}", 42) == "+42");
    CHECK(cc::format("{: }", 42) == " 42");
    CHECK(cc::format("{:+}", -42) == "-42");
    CHECK(cc::format("{:05}", 42) == "00042");
    CHECK(cc::format("{:05}", -42) == "-0042");
    CHECK(cc::format("{:+05}", 42) == "+0042");
}

TEST("format - digit grouping")
{
    // decimal groups by 3, with a customizable separator
    CHECK(cc::format("{:'}", 1232453254) == "1'232'453'254");
    CHECK(cc::format("{:,}", 1232453254) == "1,232,453,254");
    CHECK(cc::format("{:_}", 1232453254) == "1_232_453_254");
    CHECK(cc::format("{:'}", 100) == "100");
    CHECK(cc::format("{:'}", 1000) == "1'000");
    CHECK(cc::format("{:'}", -1234567) == "-1'234'567");

    // composes with sign, width/align, and zero-padding
    CHECK(cc::format("{:+'}", 1234567) == "+1'234'567");
    CHECK(cc::format("{:>12'}", 1234567) == "   1'234'567");

    // binary/hex/octal group by 4
    CHECK(cc::format("{:'x}", 0xDEADBEEF) == "dead'beef");
    CHECK(cc::format("{:'X}", 0xDEADBEEF) == "DEAD'BEEF");
    CHECK(cc::format("{:#'x}", 0xDEADBEEF) == "0xdead'beef");
    CHECK(cc::format("{:'b}", 0xFF) == "1111'1111");

    // floats: only the integer part is grouped (by 3)
    CHECK(cc::format("{:'.2f}", 1234567.5) == "1'234'567.50");
    CHECK(cc::format("{:'}", 1234567.0) == "1'234'567");
}

TEST("format - alignment and width")
{
    CHECK(cc::format("{:>5}", "ab") == "   ab");
    CHECK(cc::format("{:<5}", "ab") == "ab   ");
    CHECK(cc::format("{:^6}", "ab") == "  ab  ");
    CHECK(cc::format("{:*^7}", "ab") == "**ab***");
    CHECK(cc::format("{:>5}", 42) == "   42");
    CHECK(cc::format("{:<5}", 42) == "42   ");
}

TEST("format - strings, char, bool")
{
    CHECK(cc::format("{}", "hi") == "hi");
    CHECK(cc::format("{}", cc::string("xy")) == "xy");
    CHECK(cc::format("{}", cc::string_view("zw")) == "zw");
    CHECK(cc::format("{:.3}", "hello") == "hel");
    CHECK(cc::format("{:5}", "ab") == "ab   ");

    CHECK(cc::format("{}", 'A') == "A");
    CHECK(cc::format("{:c}", 65) == "A");
    CHECK(cc::format("{:d}", 'A') == "65");

    CHECK(cc::format("{}", true) == "true");
    CHECK(cc::format("{}", false) == "false");
    CHECK(cc::format("{:d}", true) == "1");
}

TEST("format - floats")
{
    CHECK(cc::format("{}", 0.5) == "0.5");
    CHECK(cc::format("{}", 100.0) == "100");
    CHECK(cc::format("{}", 0.5f) == "0.5");
    CHECK(cc::format("{:.2f}", 3.14159) == "3.14");
    CHECK(cc::format("{:.0f}", 3.7) == "4");
    CHECK(cc::format("{:8.2f}", 3.14159) == "    3.14");
    CHECK(cc::format("{:08.2f}", 3.14159) == "00003.14");
    CHECK(cc::format("{:.2f}", -3.14159) == "-3.14");
    CHECK(cc::format("{:+.2f}", 3.14) == "+3.14");
    CHECK(cc::format("{:.2e}", 12345.0) == "1.23e+04");
    CHECK(cc::format("{:.2E}", 12345.0) == "1.23E+04");
}

TEST("format - pointer and byte")
{
    CHECK(cc::format("{}", static_cast<void*>(nullptr)) == "0x0");
    CHECK(cc::format("{}", cc::byte{0xff}) == "0xFF");
    CHECK(cc::format("{:x}", cc::byte{0xab}) == "ab");
}

TEST("format - format_append")
{
    cc::string s = "x=";
    cc::format_append(s, "{}", 42);
    CHECK(s == "x=42");

    cc::format_append(s, ", y={}", 7);
    CHECK(s == "x=42, y=7");
}

TEST("format - string::appendf")
{
    cc::string s = "x=";
    s.appendf("{}", 42);
    CHECK(s == "x=42");
    s.appendf(", {:.2f}", 3.14159);
    CHECK(s == "x=42, 3.14");
}

TEST("format - format_to (non-allocating)")
{
    char buf[16];
    cc::isize const n = cc::format_to(cc::span<char>(buf, 16), "{}-{}", 12, 345);
    CHECK(n == 6);
    CHECK(cc::string_view(buf, n) == "12-345");

    // truncation: returns the would-be length, writes only what fits
    char small[3];
    cc::isize const m = cc::format_to(cc::span<char>(small, 3), "{}", 12345);
    CHECK(m == 5);
    CHECK(cc::string_view(small, 3) == "123");
}

TEST("format - custom types")
{
    // vec2: its own (empty) spec language, delegating components to the standard formatting
    CHECK(cc::format("{}", vec2{1, 2}) == "(1, 2)");

    // celsius: delegates the standard spec to its inner value
    CHECK(cc::format("{}", celsius{36.5}) == "36.5C");
    CHECK(cc::format("{:.1f}", celsius{36.567}) == "36.6C");
    CHECK(cc::format("{:+.1f}", celsius{36.567}) == "+36.6C");

    // member to_string()
    CHECK(cc::format("{}", greeting{}) == "hi!");
}

// =========================================================================================================
// Compile-fail cases — these MUST NOT compile. They are disabled by default and cannot be expressed as
// static_assert (a static_assert cannot assert that another expression fails to compile). To verify
// manually, define CC_FORMAT_COMPILE_FAIL_TESTS and confirm each line below fails to compile with a
// readable diagnostic. Not run in CI.
// =========================================================================================================

#if defined(CC_FORMAT_COMPILE_FAIL_TESTS)
TEST("format - compile failures (manual)")
{
    (void)cc::format("{}");                // error: argument index out of range (no args)
    (void)cc::format("{2}", 1);            // error: argument index out of range
    (void)cc::format("{} {0}", 1, 2);      // error: cannot mix automatic and explicit indexing
    (void)cc::format("{:f}", 42);          // error: invalid presentation type for integer
    (void)cc::format("{", 1);              // error: unterminated replacement field
    (void)cc::format("}", 1);              // error: single '}' in format string
    (void)cc::format("{:>5}", vec2{1, 2}); // error: vec2 takes no format spec (its own validate hook)
}
#endif
