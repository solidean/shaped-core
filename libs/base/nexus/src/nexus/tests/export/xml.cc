#include "xml.hh"

cc::string nx::impl::xml_escape(cc::string_view str)
{
    cc::string result;
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
