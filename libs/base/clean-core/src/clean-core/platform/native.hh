#pragma once

#include <clean-core/fwd.hh>


namespace cc
{
/// Demangle a C++ mangled symbol name into a human-readable form.
/// Returns the symbol unchanged when demangling fails.
/// Serialized process-wide: the platform demanglers are not thread-safe, so concurrent calls contend on a single mutex.
[[nodiscard]] cc::string demangle_symbol(cc::string_view symbol);

} // namespace cc
