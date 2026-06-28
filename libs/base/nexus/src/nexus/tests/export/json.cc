#include "json.hh"

cc::string nx::json_escape(cc::string_view str)
{
    cc::string result;
    for (auto const c : str)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char const* const hex = "0123456789abcdef";
                result += "\\u00";
                result += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                result += hex[static_cast<unsigned char>(c) & 0xF];
            }
            else
                result += c;
            break;
        }
    }
    return result;
}
