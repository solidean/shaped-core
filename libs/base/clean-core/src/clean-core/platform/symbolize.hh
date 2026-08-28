#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/platform/module_table.hh>
#include <clean-core/string/string.hh>

// Turning an address back into a name, which is the other half of cc::capture_stack.
//
// Capture is deliberately cheap and blind: it writes return addresses and never asks who they belong to, because
// asking costs orders of magnitude more than the event it would hang off.
// This is where the asking happens — at analysis time, off the hot path, and never from a crash handler.
//
// **A symbolizer resolves against a set of modules, and which set is the choice.**
// The default is this process's own, which is right for a program exporting its own trace.
// Given a recorded module table it resolves against THAT instead, in a session of its own, which is what makes a
// recording from another run — or from a process that has since died, which every crash dump is — readable at all.
//
// A foreign table is useful even when the binaries are missing.
// Failing to load one costs the function and the line and still leaves the module and the offset, so a frame degrades
// from a name to `app.exe+0x1234` rather than to nothing.

namespace cc
{
struct symbol_info;
struct symbolize_options;
struct symbolizer;
} // namespace cc

/// What a foreign symbolizer does with a module it would have to reach across the network to open.
struct cc::symbolize_options
{
    /// Whether to open a module image on a UNC path or a network drive.
    ///
    /// Off by default, and the reason is a cost rather than a correctness one.
    /// The debug-info library opens an image lazily, inside the FIRST resolve rather than at load time, so a path
    /// whose server does not answer costs a network timeout per address instead of failing — and a foreign table
    /// exists to read a recording from another machine, where such paths are the norm.
    ///
    /// Turn it on when the share is known to answer, and the binaries are only reachable there.
    /// The cost of leaving it off is names: those frames resolve to `module+offset` from the recorded table, and
    /// no debug info is consulted for them at all, a symbol server included.
    /// The alternative that needs no flag is rewriting `loaded_module::path` to a local copy before constructing —
    /// only `base` and `size` have to match the recording.
    bool load_remote_images = false;
};

/// What an address turned out to be, as far as the debug info goes.
///
/// Every field is best-effort and independently missing: a release module with no PDB resolves to a module and an
/// offset with no function, and a function without line info resolves to a name with no file.
struct cc::symbol_info
{
    /// Undecorated, so `cc::vector<int>::push_back` rather than the mangled spelling.
    /// Empty when the address is in no module this process knows, or in one with no symbols.
    cc::string function;

    /// The full path as the debug info records it, so it can be opened rather than guessed at.
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
    /// Resolves against this process's own modules.
    symbolizer();

    /// Resolves against a RECORDED set of modules instead, in a session of its own.
    ///
    /// The addresses are then interpreted exactly as the recording process meant them, whatever this process happens
    /// to have loaded and wherever it happens to have loaded it.
    /// A module whose binary cannot be found still contributes its name and the offset into it.
    ///
    /// A module whose path is a UNC path or sits on a network drive is NOT opened, and contributes name and offset
    /// only — the debug-info library would otherwise wait out a network timeout on the first resolve against it.
    /// `symbolize_options::load_remote_images` turns that off for a share known to answer.
    explicit symbolizer(cc::span<cc::loaded_module const> modules, cc::symbolize_options opts = {});

    ~symbolizer();

    symbolizer(symbolizer const&) = delete;
    symbolizer& operator=(symbolizer const&) = delete;

    /// What `address` is, or an empty result where nothing could be resolved.
    /// The reference stays valid until this symbolizer is destroyed.
    [[nodiscard]] cc::symbol_info const& resolve(void const* address);

    /// Whether this build can resolve anything at all.
    [[nodiscard]] static bool is_available();

    /// How many distinct addresses have been looked up.
    [[nodiscard]] isize cached_count() const { return _cache.size(); }

    /// Whether this resolves against a recorded table rather than against this process.
    [[nodiscard]] bool is_foreign() const { return !_modules.empty(); }

private:
    cc::map<u64, cc::symbol_info> _cache;

    /// The recorded table, empty for a symbolizer over this process.
    cc::vector<cc::loaded_module> _modules;

    /// The platform handle keying this symbolizer's own debug-info session.
    /// Null for the process session, which is shared with the crash handler and never torn down.
    void* _session = nullptr;
};
