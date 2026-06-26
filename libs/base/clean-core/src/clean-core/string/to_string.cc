#include "to_string.hh"

#include <charconv>

// Numbers go straight through std::to_chars into a small stack buffer; the result is copied into a cc::string
// (whose SSO means none of these allocate in practice). No cc::format / std::format overhead on this path.

namespace
{
template <class T>
cc::string integer_to_string(T v)
{
    char buf[24]; // 64-bit decimal: up to 20 digits + sign
    auto const r = std::to_chars(buf, buf + sizeof(buf), v);
    return cc::string(buf, cc::isize(r.ptr - buf));
}

template <class T>
cc::string float_to_string(T v)
{
    char buf[64]; // ample for shortest round-trip of float/double
    auto const r = std::to_chars(buf, buf + sizeof(buf), v);
    return cc::string(buf, cc::isize(r.ptr - buf));
}
} // namespace

cc::string cc::to_string(void const* ptr)
{
    char buf[2 + 2 * sizeof(void*)];
    buf[0] = '0';
    buf[1] = 'x';
    auto const r = std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<uintptr_t>(ptr), 16);
    return cc::string(buf, cc::isize(r.ptr - buf));
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
