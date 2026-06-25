#include "to_string.hh"

#include <clean-core/string/format.hh>

cc::string cc::to_string(void const* ptr)
{
    return cc::format("{}", ptr);
}

cc::string cc::to_string(bool b)
{
    return b ? "true" : "false";
}

cc::string cc::to_string(byte b)
{
    return cc::format("0x{:02X}", static_cast<unsigned char>(b));
}

cc::string cc::to_string(char c)
{
    return cc::string(c);
}

cc::string cc::to_string(signed char i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(unsigned char i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(signed short i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(unsigned short i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(signed int i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(unsigned int i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(signed long i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(unsigned long i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(signed long long i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(unsigned long long i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(float i)
{
    return cc::format("{}", i);
}

cc::string cc::to_string(double i)
{
    return cc::format("{}", i);
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
