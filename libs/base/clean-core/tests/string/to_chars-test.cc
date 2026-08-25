#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
template <class T, class... Args>
cc::string_view chars_of(cc::span<char> buf, T v, Args... args)
{
    auto const n = cc::to_chars(buf, v, args...);
    return cc::string_view(buf.data(), n);
}
} // namespace

TEST("to_chars - integers")
{
    char buf[cc::to_chars_int_max];

    CHECK(chars_of(buf, 0) == "0");
    CHECK(chars_of(buf, -1) == "-1");
    CHECK(chars_of(buf, 1234567) == "1234567");
    CHECK(chars_of(buf, (unsigned long long)(18446744073709551615ull)) == "18446744073709551615");
    CHECK(chars_of(buf, (long long)(-9223372036854775807ll - 1)) == "-9223372036854775808");

    // the char-sized overloads render as numbers, not as characters
    CHECK(chars_of(buf, (signed char)(-5)) == "-5");
    CHECK(chars_of(buf, (unsigned char)(200)) == "200");
}

TEST("to_chars - floats")
{
    char buf[cc::to_chars_float_max];

    SECTION("shortest round-trip is the default")
    {
        CHECK(chars_of(buf, 0.5) == "0.5");
        CHECK(chars_of(buf, 1.0) == "1");
        CHECK(chars_of(buf, 0.1) == "0.1"); // not 0.100000000000000006
        CHECK(chars_of(buf, -2.25f) == "-2.25");
    }

    SECTION("notations honor the precision")
    {
        CHECK(chars_of(buf, 1.5, cc::float_notation::fixed, 3) == "1.500");
        CHECK(chars_of(buf, 1.5, cc::float_notation::scientific, 2) == "1.50e+00");
        CHECK(chars_of(buf, 1500.0, cc::float_notation::general, 2) == "1.5e+03");
    }

    SECTION("a negative precision means the notation's own default")
    {
        CHECK(chars_of(buf, 1.5, cc::float_notation::fixed) == "1.500000");  // 6 digits
        CHECK(chars_of(buf, 1.5, cc::float_notation::shortest, 3) == "1.5"); // shortest ignores it
    }
}

TEST("to_chars - a big precision outgrows to_chars_float_max")
{
    // The integer part of a double is up to 309 digits on its own, so the fraction the precision asks for is what
    // pushes the requirement past the fixed buffer — which is the whole reason to_chars_size exists.
    CHECK(cc::to_chars_size(cc::float_notation::shortest, 4000) == cc::to_chars_float_max); // precision does not apply
    CHECK(cc::to_chars_size(cc::float_notation::fixed, 400) > cc::to_chars_float_max);
    CHECK(cc::to_chars_size(cc::float_notation::fixed, -1) <= cc::to_chars_float_max + 12); // the default is 6 digits

    auto const precision = 400;
    auto buf = cc::vector<char>::create_uninitialized(cc::to_chars_size(cc::float_notation::fixed, precision));
    auto const n = cc::to_chars(buf, 1.0, cc::float_notation::fixed, precision);

    CHECK(n == 1 + 1 + precision); // "1" + "." + the digits
    CHECK(cc::string_view(buf.data(), n).subview({.offset = 0, .size = 4}) == "1.00");
}
