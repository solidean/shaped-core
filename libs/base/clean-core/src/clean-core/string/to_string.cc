#include "to_string.hh"

#include <clean-core/common/assert.hh>

#include <charconv>

using namespace cc::primitive_defines;

// Numbers go straight through std::to_chars into a small stack buffer, then into a cc::string.
// On 64-bit every result they can produce fits inline, so none of these allocate.
// The path carries none of cc::format's dispatch or spec parsing.

namespace
{
template <class T>
cc::string integer_to_string(T v)
{
    char buf[24]; // 64-bit decimal: up to 20 digits + sign
    auto const r = std::to_chars(buf, buf + sizeof(buf), v);
    return cc::string(buf, isize(r.ptr - buf));
}

template <class T>
cc::string float_to_string(T v)
{
    char buf[64]; // ample for shortest round-trip of float/double
    auto const r = std::to_chars(buf, buf + sizeof(buf), v);
    return cc::string(buf, isize(r.ptr - buf));
}
} // namespace

cc::string cc::to_string(void const* ptr)
{
    char buf[2 + 2 * sizeof(void*)];
    buf[0] = '0';
    buf[1] = 'x';
    auto const r = std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<uintptr_t>(ptr), 16);
    return cc::string(buf, isize(r.ptr - buf));
}

cc::string cc::to_string(bool b)
{
    return b ? "true" : "false";
}

cc::string cc::to_string(byte b)
{
    auto const hex = [](unsigned d) { return d < 10 ? char('0' + d) : char('A' + (d - 10)); };
    unsigned const v = static_cast<unsigned char>(b);
    char const buf[4] = {'0', 'x', hex((v >> 4) & 0xF), hex(v & 0xF)};
    return cc::string(buf, 4);
}

cc::string cc::to_string(char c)
{
    return cc::string(c);
}

cc::string cc::to_string(signed char i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(unsigned char i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(signed short i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(unsigned short i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(signed int i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(unsigned int i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(signed long i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(unsigned long i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(signed long long i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(unsigned long long i)
{
    return integer_to_string(i);
}

cc::string cc::to_string(float i)
{
    return float_to_string(i);
}

cc::string cc::to_string(double i)
{
    return float_to_string(i);
}

cc::string cc::to_string(char const* s)
{
    return {s};
}

cc::string cc::to_string(string s)
{
    return s;
}

cc::string cc::to_string(string_view s)
{
    return cc::string(s.data(), s.size());
}

// -----------------------------------------------------------------------------------------------------
// cc::to_chars — the same std::to_chars calls, straight into the caller's buffer.

namespace
{
template <class T>
isize float_to_chars(cc::span<char> out, T v, cc::float_notation notation, int precision)
{
    auto* const first = out.data();
    auto* const last = first + out.size();

    std::to_chars_result r;
    if (notation == cc::float_notation::shortest)
        r = std::to_chars(first, last, v); // shortest round-trip; precision does not apply
    else
    {
        auto const fmt = notation == cc::float_notation::fixed      ? std::chars_format::fixed
                       : notation == cc::float_notation::scientific ? std::chars_format::scientific
                                                                    : std::chars_format::general;
        r = std::to_chars(first, last, v, fmt, precision < 0 ? 6 : precision); // 6 digits, matching std::format
    }

    // A failed to_chars leaves the buffer unspecified and reports `last` as its end, so returning the distance would
    // hand back that many unspecified chars rather than a short write.
    CC_ASSERT(r.ec == std::errc(), "cc::to_chars: buffer too small; size it off cc::to_chars_size(notation, "
                                   "precision)");
    return isize(r.ptr - first);
}

template <class T>
isize integer_to_chars(cc::span<char> out, T v)
{
    auto* const first = out.data();
    auto const r = std::to_chars(first, first + out.size(), v);
    CC_ASSERT(r.ec == std::errc(), "cc::to_chars: buffer too small");
    return isize(r.ptr - first);
}
} // namespace

isize cc::to_chars(span<char> out, float v, float_notation notation, int precision)
{
    return float_to_chars(out, v, notation, precision);
}

isize cc::to_chars(span<char> out, double v, float_notation notation, int precision)
{
    return float_to_chars(out, v, notation, precision);
}

isize cc::to_chars(span<char> out, signed char v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, unsigned char v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, signed short v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, unsigned short v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, signed int v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, unsigned int v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, signed long v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, unsigned long v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, signed long long v)
{
    return integer_to_chars(out, v);
}
isize cc::to_chars(span<char> out, unsigned long long v)
{
    return integer_to_chars(out, v);
}
