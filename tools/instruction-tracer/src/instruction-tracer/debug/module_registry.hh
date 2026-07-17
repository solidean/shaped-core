#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// One image loaded in the debuggee.
struct module_info
{
    u64 base = 0;
    u64 size = 0;
    cc::string path; // full path as the loader reported it
    cc::string name; // file name only, e.g. "mymodule.exe"

    bool contains(u64 address) const { return address >= base && address < base + size; }
};

/// The debuggee's loaded images, tracked from the debug loop's load/unload events. Address ranges
/// never overlap, so rip -> module is unambiguous.
class module_registry
{
public:
    void add(module_info module);
    void remove(u64 base);

    /// The module whose address range covers `address`, if any.
    module_info const* find_by_address(u64 address) const;

    /// The module whose file name matches `name`, case-insensitively. A bare stem also matches
    /// ("mymodule" finds "mymodule.exe"), which is what people type.
    module_info const* find_by_name(cc::string_view name) const;

    cc::span<module_info const> all() const { return _modules; }

private:
    cc::vector<module_info> _modules;
};

/// The file-name portion of a path, after the last '/' or '\'.
cc::string_view path_file_name(cc::string_view path);
} // namespace itrace
