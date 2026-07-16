#pragma once

#include <instruction-tracer/debug/symbol_session.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/decode/instruction_decoder.hh>

namespace itrace
{
/// Decode and symbolize a recorded trace: instruction text, source file/line, where each taken
/// transfer landed, and — with `want_owner` — the function containing each instruction. Runs after
/// collection, never inside the single-step loop — symbol lookups hit the PDB and would otherwise
/// cost more than the tracing.
void enrich_trace(trace& t,
                  symbol_session const& symbols,
                  instruction_decoder const& decoder,
                  bool want_source,
                  bool want_owner);
} // namespace itrace
