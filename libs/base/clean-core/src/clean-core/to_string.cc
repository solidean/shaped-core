#include "to_string.hh"

#include <format>

cc::string cc::to_string(void const* ptr)
{
    return std::format("{}", ptr);
}

cc::string cc::to_string(bool b)
{
    return b ? "true" : "false";
}

cc::string cc::to_string(byte b)
{
    return std::format("0x{:02X}", static_cast<unsigned char>(b));
}

cc::string cc::to_string(char c)
{
    return cc::string(c);
}

cc::string cc::to_string(signed char i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(unsigned char i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(signed short i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(unsigned short i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(signed int i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(unsigned int i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(signed long i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(unsigned long i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(signed long long i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(unsigned long long i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(float i)
{
    return std::format("{}", i);
}

cc::string cc::to_string(double i)
{
    return std::format("{}", i);
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
