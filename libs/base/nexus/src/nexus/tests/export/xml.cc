#include "xml.hh"

std::string nx::impl::xml_escape(std::string_view str)
{
    std::string result;
    result.reserve(str.size());

    for (auto c : str)
    {
        switch (c)
        {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += c; break;
        }
    }

    return result;
}
