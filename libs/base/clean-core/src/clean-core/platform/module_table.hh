#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

// Which binaries were mapped where, so an address recorded by one process means something to another.
//
// A captured stack is addresses, and an address only means anything against the module it fell in.
// Inside the recording process that is implicit — the modules are still loaded — and the moment a recording travels,
// to another machine or merely past the process's death, it is the missing half.
// **A crash dump is exactly that case**, so a dump without this is a column of hex nobody can ever resolve.
//
// The identity matters as much as the base.
// Two builds of the same path are different binaries, and resolving against the wrong one produces confident nonsense
// rather than an error, so a consumer checks identity before it trusts a name.

namespace cc
{
struct loaded_module;

/// Every module mapped into this process, in no particular order.
/// Empty where the platform cannot be asked, which `module_enumeration_available` reports up front.
[[nodiscard]] cc::vector<cc::loaded_module> enumerate_loaded_modules();

/// Whether this build can enumerate modules at all.
[[nodiscard]] bool module_enumeration_available();
} // namespace cc

/// One mapped binary, and enough to find the same one again elsewhere.
struct cc::loaded_module
{
    /// Where it was mapped, which is what a recorded address is relative to.
    /// ASLR moves this between runs, so it is meaningless without the recording it came from.
    u64 base = 0;
    u64 size = 0;

    /// The path the OS reported, which is where to look for the binary and its debug info first.
    cc::string path;

    /// What identifies this exact build, in whatever form the platform's symbol servers key on.
    ///
    /// On Windows that is the PE `TimeDateStamp` and `SizeOfImage` concatenated as hex — literally the key a symbol
    /// server URL is built from — and on ELF it would be the build id.
    /// Empty when it could not be read, which means a consumer can locate the module but cannot prove it is the right
    /// build of it.
    cc::string identity;

    [[nodiscard]] bool contains(u64 address) const { return address >= base && address < base + size; }

    /// The file name of `path`, for a display that does not want an install directory.
    [[nodiscard]] cc::string_view name() const;
};
