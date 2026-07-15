#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/cli/target_spec.hh>
#include <instruction-tracer/debug/module_registry.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// A symbol found by name lookup.
struct symbol_match
{
    u64 address = 0;
    cc::string name;
    cc::string module;
};

/// A source file and line from the PDB.
struct source_line
{
    cc::string file;
    u32 line = 0;
};

/// Why a symbol lookup failed. `candidates` is non-empty exactly when the spec was ambiguous.
struct symbol_error
{
    cc::string message;
    cc::vector<symbol_match> candidates;
};

/// dbghelp lifetime and queries against a live debuggee. All calls must happen on the debug-loop
/// thread — dbghelp is not reentrant. Owns nothing but the dbghelp session; module storage lives in
/// the registry passed at construction, which must outlive this.
class symbol_session
{
public:
    /// `process` is the debuggee's HANDLE. Symbols are loaded per module, not by invading the
    /// process, so this must be constructed while the debuggee is stopped at its first event.
    symbol_session(void* process, module_registry const& modules);
    ~symbol_session();

    symbol_session(symbol_session const&) = delete;
    symbol_session& operator=(symbol_session const&) = delete;

    /// `file_handle` is the debug event's hFile, or null; dbghelp reads the PDB path through it.
    void on_module_loaded(module_info const& module, void* file_handle);
    void on_module_unloaded(u64 base);

    /// Resolve a target to a runtime address. Tries an exact name first, then a substring sweep;
    /// more than one distinct hit fails with every candidate listed.
    cc::result<u64, symbol_error> resolve(target_spec const& spec) const;

    cc::optional<symbol_match> symbol_at(u64 address) const;
    cc::optional<source_line> line_at(u64 address) const;

    /// Physical frames only — inline frames are not expanded. `context` is a CONTEXT const*, copied
    /// before the walk (StackWalk64 mutates it).
    cc::vector<stack_frame> walk_stack(void* thread_handle, void const* context) const;

    /// "module.exe!foo::bar+0x12", falling back to a bare address where no symbol is known.
    cc::string describe(u64 address) const;

private:
    void* _process = nullptr;
    module_registry const& _modules;
};
} // namespace itrace
