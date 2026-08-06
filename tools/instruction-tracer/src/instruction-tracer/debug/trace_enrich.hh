#pragma once

#include <instruction-tracer/debug/symbol_session.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/decode/instruction_decoder.hh>

namespace itrace
{
/// Decode and symbolize a recorded trace: instruction text, source file/line, and where each taken transfer landed.
/// `want_memory` also resolves every memory access to an effective address, classifies its region by walking the frames in order, and symbolizes it.
/// That last one needs the register snapshots and stack bounds captured live.
/// Either of `want_owner` and `want_memory` fills the function containing each instruction, since the memory attribution needs it too.
///
/// Runs after collection, never inside the single-step loop: symbol lookups hit the PDB and would cost more than the tracing itself.
void enrich_trace(trace& t,
                  symbol_session const& symbols,
                  instruction_decoder const& decoder,
                  bool want_source,
                  bool want_owner,
                  bool want_memory);
} // namespace itrace
