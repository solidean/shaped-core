#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>

// Turning an address back into a name, which is the other half of cc::capture_stack.
//
// Capture is deliberately cheap and blind: it writes return addresses and never asks who they belong to, because
// asking costs orders of magnitude more than the event it would hang off.
// This is where the asking happens — at analysis time, off the hot path, and never from a crash handler.
//
// **It resolves against THIS process's loaded modules.**
// A recording made by another process, or by an earlier run under a different ASLR layout, mostly resolves to nothing
// and reports it: the addresses simply are not in any module this process has loaded.
// Symbolizing a foreign recording needs its module base table and its binaries, which is a separate mechanism.

namespace cc
{
struct symbol_info;
struct symbolizer;
} // namespace cc

/// What an address turned out to be, as far as the debug info goes.
///
/// Every field is best-effort and independently missing: a release module with no PDB resolves to a module and an
/// offset with no function, and a function without line info resolves to a name with no file.
struct cc::symbol_info
{
    /// Undecorated, so `cc::vector<int>::push_back` rather than the mangled spelling.
    /// Empty when the address is in no module this process knows, or in one with no symbols.
    cc::string function;

    cc::string file;

    /// The module's file name rather than its path — `app.exe`, not where it was installed from.
    cc::string module;

    i32 line = 0;

    /// Bytes into the function, which is what makes a name useful in an optimized build where one name covers a lot
    /// of inlined ground.
    u64 displacement = 0;

    /// Bytes into the module, and the fallback that still finds a frame in a disassembly when nothing else resolves.
    u64 module_offset = 0;

    [[nodiscard]] bool has_function() const { return !function.empty(); }
    [[nodiscard]] bool has_line() const { return !file.empty() && line > 0; }

    /// A single line for a human: the function, then where it came from, then whatever is left when neither is known.
    [[nodiscard]] cc::string to_string() const;
};

/// Resolves addresses, and remembers what it found.
///
/// The cache is the point rather than an optimization: a sampled profile is thousands of hits on a handful of
/// addresses, and a debug-info lookup is milliseconds.
///
/// **Not thread-safe, and neither is the platform underneath it** — Windows' DbgHelp requires callers to serialize.
/// One symbolizer per thread, or one behind a lock.
struct cc::symbolizer
{
    symbolizer();

    symbolizer(symbolizer const&) = delete;
    symbolizer& operator=(symbolizer const&) = delete;

    /// What `address` is, or an empty result where nothing could be resolved.
    /// The reference stays valid until this symbolizer is destroyed.
    [[nodiscard]] cc::symbol_info const& resolve(void const* address);

    /// Whether this build can resolve anything at all.
    [[nodiscard]] static bool is_available();

    /// How many distinct addresses have been looked up, and how many of those resolved to a function.
    [[nodiscard]] isize cached_count() const { return _cache.size(); }

private:
    cc::map<u64, cc::symbol_info> _cache;
};
