#include "greeting.hh"

cc::string __ns__::greeting(cc::string_view name)
{
    auto result = cc::string("hello, ");
    result += name;
    return result;
}
