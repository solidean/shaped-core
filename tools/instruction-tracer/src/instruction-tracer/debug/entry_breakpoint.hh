#pragma once

#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// The 0xCC patch at the traced function's entry.
///
/// An int3 cannot stay armed while the byte it replaced executes, so every hit runs the same dance:
/// rewind rip past the trap, disarm, single-step the real instruction, re-arm. arm()/disarm() are
/// the two halves; the debug loop sequences them.
class entry_breakpoint
{
public:
    /// `process` is the debuggee's HANDLE.
    entry_breakpoint(void* process, u64 address);

    entry_breakpoint(entry_breakpoint const&) = delete;
    entry_breakpoint& operator=(entry_breakpoint const&) = delete;

    /// Save the original byte and write 0xCC. Idempotent. False if the memory could not be written.
    bool arm();

    /// Restore the original byte. Idempotent.
    bool disarm();

    u64 address() const { return _address; }
    bool is_armed() const { return _armed; }

private:
    void* _process = nullptr;
    u64 _address = 0;
    u8 _original = 0;
    bool _armed = false;
};
} // namespace itrace
