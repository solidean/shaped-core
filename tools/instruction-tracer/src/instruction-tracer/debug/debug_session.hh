#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/cli/target_spec.hh>
#include <instruction-tracer/debug/entry_breakpoint.hh>
#include <instruction-tracer/debug/module_registry.hh>
#include <instruction-tracer/debug/symbol_session.hh>
#include <instruction-tracer/debug/trace_session.hh>

namespace itrace
{
struct debug_config
{
    cc::string exe;
    cc::vector<cc::string> args;
    target_spec target;

    u64 skip = 0;
    u32 traces = 1;
    trace_config trace;
    bool terminate_after_traces = true;
};

/// Launches a process under the Win32 debug API and records the traces the config asks for.
///
/// One invocation per instance; run() drives the debuggee to completion (or kills it once the last
/// trace lands) and hands back what it recorded.
class debug_session
{
public:
    explicit debug_session(debug_config config);
    ~debug_session();

    debug_session(debug_session const&) = delete;
    debug_session& operator=(debug_session const&) = delete;

    /// `on_trace` is called for each completed trace while the debuggee — and therefore its symbol
    /// session — is still alive; that is the only window in which a trace can be symbolized.
    ///
    /// Fails if the process cannot be launched, or if the target never resolves (with candidates,
    /// when the spec was ambiguous). Returns fewer traces than asked for if the debuggee exited
    /// first — that is not an error.
    cc::result<cc::vector<trace>, symbol_error> run(cc::function_ref<void(trace&, symbol_session const&)> on_trace);

private:
    /// Where the loop is with respect to the entry breakpoint.
    enum class state
    {
        waiting_for_entry,
        /// Single-stepping the instruction the int3 displaced, so it can be re-armed behind us.
        stepping_off_breakpoint,
        tracing,
        finished,
    };

    // Event handlers. Each returns the DBG_* continue status for the event.
    u32 on_create_process(void const* info, u32 thread_id);
    u32 on_load_dll(void const* info);
    u32 on_exception(void const* info, u32 thread_id);
    u32 on_breakpoint_hit(u32 thread_id, void* thread);
    u32 on_single_step(u32 thread_id, void* thread);

    /// Try to resolve the target and arm the breakpoint. Idempotent; a symbol may only appear once
    /// its DLL loads, so this is retried on every module load until it succeeds.
    void try_resolve_and_arm();

    /// Track a newly loaded image and hand it to dbghelp. Takes ownership of `file_handle`.
    void register_module(void* file_handle, u64 base);
    void unregister_module(u64 base);

    /// Hand the finished trace to `_on_trace` for symbolization, bank it, and either re-arm for the
    /// next one or stop.
    void finish_trace();

    debug_config _config;

    void* _process = nullptr;
    void* _main_thread = nullptr;
    cc::map<u32, void*> _threads;

    /// Each loaded module's image handle, kept open until the module unloads. SYMOPT_DEFERRED_LOADS
    /// means dbghelp reads the PDB on the first query rather than at SymLoadModuleEx, and it reads
    /// it through this handle — closing it early makes every later lookup silently miss.
    cc::map<u64, void*> _module_files;

    module_registry _modules;
    cc::unique_ptr<symbol_session> _symbols;
    cc::unique_ptr<entry_breakpoint> _breakpoint;
    cc::unique_ptr<trace_session> _tracer;

    state _state = state::waiting_for_entry;
    bool _resolved = false;
    u64 _hit_index = 0;
    u32 _traced_thread = 0;

    cc::vector<trace> _traces;
    cc::optional<symbol_error> _resolve_error;

    /// Bound by run() to its caller's callback, which outlives the call. Non-owning — never give it
    /// a temporary. Valid only for the duration of run().
    cc::function_ref<void(trace&, symbol_session const&)> _on_trace;
};
} // namespace itrace
