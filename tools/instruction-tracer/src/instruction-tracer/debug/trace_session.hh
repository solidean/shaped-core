#pragma once

#include <clean-core/common/utility.hh>
#include <instruction-tracer/debug/symbol_session.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// Stop conditions and budget for one recorded invocation.
struct trace_config
{
    u32 max_instructions = 100;
    bool until_return = true;
    bool stop_at_syscall = true;
    bool capture_registers = false;
    bool capture_stack = true;
};

/// Records the instructions one invocation actually retires.
///
/// Deliberately knows nothing about disassembly or source: it captures rip, bytes and registers,
/// and the trace is enriched afterwards. The only decode it does is the raw-byte syscall check,
/// which has to happen before the step that would enter the kernel.
class trace_session
{
public:
    trace_session(void* process, trace_config const& config);

    /// Start recording at an entry-breakpoint hit. `context` is a CONTEXT const* whose rip is the
    /// function's first instruction (already rewound past the trap) and whose rsp still points at
    /// the return address — the prologue has not run yet.
    void begin(u32 index,
               u64 hit_index,
               u32 thread_id,
               void* thread_handle,
               void const* context,
               symbol_session const& symbols);

    /// Feed one single-step event. Returns true while more stepping is wanted.
    bool on_step(void const* context);

    /// Stop for a reason the stepper cannot see (a fault, or the process going away).
    void abort(step_reason reason);

    bool is_active() const { return _active; }
    u32 thread_id() const { return _trace.thread_id; }

    /// Move the finished trace out. Only valid once is_active() is false.
    trace take() { return cc::move(_trace); }

private:
    /// Append a record for the instruction at `context`'s rip. False if its bytes are unreadable.
    bool record_at(void const* context);

    void* _process = nullptr;
    trace_config _config;

    trace _trace;
    bool _active = false;
    u64 _entry_rsp = 0;
};
} // namespace itrace
