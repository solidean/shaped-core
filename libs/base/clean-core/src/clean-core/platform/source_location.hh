#pragma once

#include <source_location>

namespace cc
{
/// std::source_location under the cc name, so clean-core code spells one namespace.
using source_location = std::source_location;
} // namespace cc
