#pragma once

#include <clean-core/container/span.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/fwd.hh>

namespace itrace
{
class instruction_decoder;
} // namespace itrace

/// Turns recorded instruction bytes into text via Zydis.
/// Pure: no debuggee, no symbols, no I/O — the bytes were captured live, so decoding them is just a function.
class itrace::instruction_decoder
{
public:
    /// Decode one instruction's bytes, filling text/category and the is_atomic / is_indirect / reads_memory / writes_memory facts.
    /// `length` stays 0, `text` empty and the flags false when the bytes do not decode.
    void decode_one(recorded_instruction& insn) const;

    /// decode_one over a whole trace.
    void decode(cc::span<recorded_instruction> instructions) const;
};

namespace itrace
{

/// True for `syscall` / `sysenter` / `int 0x2e` — the user-to-kernel transitions we refuse to single-step through.
/// Matches raw bytes, so it works before anything is decoded.
bool is_syscall_bytes(cc::span<u8 const> bytes);
} // namespace itrace
