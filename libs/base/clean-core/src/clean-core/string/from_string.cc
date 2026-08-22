#include "from_string.hh"

#include <charconv>

// std::from_chars already rejects most of what cc::to_string never produces: it takes no leading '+',
// no surrounding whitespace and no "0x" prefix, and it refuses a '-' for an unsigned type.
// Requiring it to land exactly on the end of the view closes the rest, so "12abc" and "12 " fail.
// A value too large for the target comes back as result_out_of_range and fails the same way.

namespace
{
/// One uppercase hex digit as its value, or -1.
/// Uppercase only, because cc::to_string(byte) writes uppercase and this is its exact inverse.
int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

template <class T>
bool parse_through_from_chars(cc::string_view s, T& out)
{
    if (s.empty())
        return false;

    auto const* const first = s.data();
    auto const* const last = s.data() + s.size();

    T value = {};
    auto const [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last)
        return false;

    out = value;
    return true;
}
} // namespace

bool cc::from_string(string_view s, bool& out)
{
    if (s == "true")
        return out = true, true;
    if (s == "false")
        return out = false, true;
    return false;
}

bool cc::from_string(string_view s, char& out)
{
    if (s.size() != 1)
        return false;

    out = s[0];
    return true;
}

bool cc::from_string(string_view s, byte& out)
{
    if (s.size() != 4 || s[0] != '0' || s[1] != 'x')
        return false;

    auto const high = hex_digit(s[2]);
    auto const low = hex_digit(s[3]);
    if (high < 0 || low < 0)
        return false;

    out = byte(high * 16 + low);
    return true;
}

bool cc::from_string(string_view s, signed char& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, unsigned char& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, signed short& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, unsigned short& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, signed int& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, unsigned int& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, signed long& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, unsigned long& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, signed long long& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, unsigned long long& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, float& out)
{
    return parse_through_from_chars(s, out);
}

bool cc::from_string(string_view s, double& out)
{
    return parse_through_from_chars(s, out);
}
