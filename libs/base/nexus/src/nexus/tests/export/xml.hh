#pragma once

#include <string>
#include <string_view>

namespace nx::impl
{
/// Escapes the five XML predefined entities (&, <, >, ", ') so a raw string can
/// be embedded in element text or an attribute value. Shared by the Catch2 and
/// JUnit exporters.
std::string xml_escape(std::string_view str);
} // namespace nx::impl
