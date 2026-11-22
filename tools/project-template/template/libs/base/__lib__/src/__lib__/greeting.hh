#pragma once

#include <__lib__/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace __ns__
{
/// "hello, <name>"; the placeholder the project template starts from, to be replaced by the first real library.
cc::string greeting(cc::string_view name);
} // namespace __ns__
