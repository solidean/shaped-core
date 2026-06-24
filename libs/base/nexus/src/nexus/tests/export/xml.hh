#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace nx::impl
{
/// Escapes the five XML predefined entities (&, <, >, ", ') so a raw string can
/// be embedded in element text or an attribute value. Shared by the Catch2 and
/// JUnit exporters.
cc::string xml_escape(cc::string_view str);
} // namespace nx::impl
