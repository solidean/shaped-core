#pragma once

#include <clean-core/fwd.hh>


namespace cc
{
/// Demangle a C++ mangled symbol name into a human-readable form.
/// Returns the symbol unchanged when demangling fails.
/// A name that cannot be a mangling (an MSVC one carries a '?', an Itanium one only [A-Za-z0-9_$.]) returns at once, taking no lock.
/// Everything else is serialized process-wide: the platform demanglers are not thread-safe, so concurrent calls contend on a single mutex.
/// On Windows that mutex is the DbgHelp lock a symbolizer holds while it loads a PDB.
[[nodiscard]] cc::string demangle_symbol(cc::string_view symbol);

} // namespace cc
