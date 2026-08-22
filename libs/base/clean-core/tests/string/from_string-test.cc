#include <clean-core/string/from_string.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// The optional form, so a CHECK can read as one expression.
template <class T>
bool parses_to(cc::string_view s, T expected)
{
    auto const v = cc::from_string<T>(s);
    return v.has_value() && v.value() == expected;
}

template <class T>
bool rejects(cc::string_view s)
{
    return !cc::from_string<T>(s).has_value();
}
} // namespace

TEST("from_string - integers")
{
    CHECK(parses_to<int>("0", 0));
    CHECK(parses_to<int>("42", 42));
    CHECK(parses_to<int>("-42", -42));
    CHECK(parses_to<int>("2147483647", 2147483647));
    CHECK(parses_to<int>("-2147483648", -2147483648));

    CHECK(parses_to<i64>("9223372036854775807", 9223372036854775807LL));
    CHECK(parses_to<u64>("18446744073709551615", 18446744073709551615ULL));

    SECTION("the narrow types are numeric, matching to_string")
    {
        CHECK(parses_to<signed char>("-128", static_cast<signed char>(-128)));
        CHECK(parses_to<unsigned char>("255", static_cast<unsigned char>(255)));
        CHECK(parses_to<short>("-32768", static_cast<short>(-32768)));
    }
}

TEST("from_string - integer rejections")
{
    SECTION("nothing to parse")
    {
        CHECK(rejects<int>(""));
        CHECK(rejects<int>("-"));
        CHECK(rejects<int>("abc"));
    }

    SECTION("the whole view must be consumed")
    {
        CHECK(rejects<int>("12abc"));
        CHECK(rejects<int>("12 "));
        CHECK(rejects<int>("12.5"));
        CHECK(rejects<int>("1,000"));
    }

    SECTION("no surrounding whitespace")
    {
        CHECK(rejects<int>(" 12"));
        CHECK(rejects<int>("\t12"));
    }

    SECTION("no leading plus — to_string never produces one")
    {
        CHECK(rejects<int>("+12"));
    }

    SECTION("no base prefixes")
    {
        CHECK(rejects<int>("0x1f"));
        CHECK(rejects<int>("0b101"));
    }

    SECTION("a value the type cannot hold fails rather than wrapping")
    {
        CHECK(rejects<signed char>("300"));
        CHECK(rejects<signed char>("-129"));
        CHECK(rejects<unsigned char>("256"));
        CHECK(rejects<int>("2147483648"));
        CHECK(rejects<u64>("18446744073709551616"));
    }

    SECTION("unsigned takes no sign")
    {
        CHECK(rejects<unsigned int>("-1"));
    }
}

TEST("from_string - floats")
{
    CHECK(parses_to<float>("0", 0.f));
    CHECK(parses_to<float>("1.5", 1.5f));
    CHECK(parses_to<float>("-1.5", -1.5f));
    CHECK(parses_to<double>("1e10", 1e10));
    CHECK(parses_to<double>("1.25e-3", 1.25e-3));

    SECTION("the spellings to_chars produces round-trip")
    {
        auto const inf = cc::from_string<double>("inf");
        REQUIRE(inf.has_value());
        CHECK(inf.value() > 0);
        CHECK(inf.value() == inf.value() * 2); // only infinity is its own double

        auto const nan = cc::from_string<double>("nan");
        REQUIRE(nan.has_value());
        CHECK(nan.value() != nan.value()); // only NaN is unequal to itself
    }

    SECTION("same strictness as the integers")
    {
        CHECK(rejects<float>(""));
        CHECK(rejects<float>("1.5x"));
        CHECK(rejects<float>(" 1.5"));
        CHECK(rejects<float>("+1.5"));
        CHECK(rejects<float>("1,5"));
    }
}

TEST("from_string - bool is true/false only")
{
    CHECK(parses_to<bool>("true", true));
    CHECK(parses_to<bool>("false", false));

    SECTION("the friendlier CLI spellings are a caller's business, not this one's")
    {
        CHECK(rejects<bool>("1"));
        CHECK(rejects<bool>("0"));
        CHECK(rejects<bool>("yes"));
        CHECK(rejects<bool>("on"));
    }

    SECTION("case-sensitive")
    {
        CHECK(rejects<bool>("True"));
        CHECK(rejects<bool>("TRUE"));
    }

    SECTION("still the whole view")
    {
        CHECK(rejects<bool>("truex"));
        CHECK(rejects<bool>("true "));
        CHECK(rejects<bool>(""));
    }
}

TEST("from_string - char is exactly one character")
{
    CHECK(parses_to<char>("a", 'a'));
    CHECK(parses_to<char>("7", '7'));
    CHECK(parses_to<char>(" ", ' '));

    // Not a number, and not a string either: to_string(char) writes the character itself, so this reads it back.
    CHECK(rejects<char>(""));
    CHECK(rejects<char>("ab"));
}

TEST("from_string - byte is the 0xAF spelling to_string writes")
{
    CHECK(parses_to<byte>("0x00", byte(0)));
    CHECK(parses_to<byte>("0x0F", byte(15)));
    CHECK(parses_to<byte>("0xAF", byte(0xAF)));
    CHECK(parses_to<byte>("0xFF", byte(0xFF)));

    // Uppercase only, and both digits always: the strictness IS the contract, since anything else would
    // accept a spelling to_string never produces.
    CHECK(rejects<byte>("0xaf"));
    CHECK(rejects<byte>("0xF"));
    CHECK(rejects<byte>("AF"));
    CHECK(rejects<byte>("0XAF"));
    CHECK(rejects<byte>("0xAFF"));
    CHECK(rejects<byte>("0xG0"));
}

TEST("from_string - out parameter is untouched on failure")
{
    auto value = 7;
    CHECK(!cc::from_string("nope", value));
    CHECK(value == 7);

    CHECK(cc::from_string("9", value));
    CHECK(value == 9);
}

TEST("from_string - round-trips whatever to_string wrote")
{
    auto const check_int = [](i64 v) { return parses_to<i64>(cc::to_string(v), v); };
    CHECK(check_int(0));
    CHECK(check_int(1));
    CHECK(check_int(-1));
    CHECK(check_int(1234567890));
    CHECK(check_int(-9223372036854775807LL - 1));

    auto const check_double = [](double v) { return parses_to<double>(cc::to_string(v), v); };
    CHECK(check_double(0.0));
    CHECK(check_double(1.0));
    CHECK(check_double(-0.5));
    CHECK(check_double(1e-300));
    CHECK(check_double(1.7976931348623157e308));

    CHECK(parses_to<bool>(cc::to_string(true), true));
    CHECK(parses_to<bool>(cc::to_string(false), false));

    CHECK(parses_to<char>(cc::to_string('q'), 'q'));

    // Every byte, since that overload's whole job is to undo one specific four-character spelling.
    for (auto v = 0; v < 256; ++v)
        CHECK(parses_to<byte>(cc::to_string(byte(v)), byte(v)));
}
