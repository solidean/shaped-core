#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace nx
{
/// Escapes a string for embedding in a JSON string literal: quotes and backslashes are backslash-escaped,
/// the common whitespace controls become \n / \r / \t, and any other control char below 0x20 becomes a
/// \u00XX escape. Shared by the JSON sidecar writers (perf metrics, test listing).
cc::string json_escape(cc::string_view str);
} // namespace nx
