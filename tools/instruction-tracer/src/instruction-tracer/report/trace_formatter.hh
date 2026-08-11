#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/symbol_session.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/fwd.hh>
#include <instruction-tracer/report/source_cache.hh>

namespace itrace
{
struct format_options;
} // namespace itrace

/// Which optional blocks the trace section prints, straight off `--stack` / `--source` / `--register-diffs`.
struct itrace::format_options
{
    bool stack = true;
    bool source = true;
    bool register_diffs = false;
};

namespace itrace
{

/// Render one enriched trace: the stack at entry, then the retired instructions grouped under the
/// source lines they came from, with branch annotation derived from where control actually went.
/// Pure — everything it prints was resolved during enrichment.
cc::string format_trace(trace const& t, u32 total_traces, format_options const& opts, source_cache& sources);

/// The "symbol X is ambiguous" report, listing every candidate.
cc::string format_symbol_error(symbol_error const& error);

/// One line per instruction, without the source heading format_trace groups them under.
cc::string format_instruction(recorded_instruction const& insn);

/// "00007ff6`11203410" — the grouped form debuggers print.
cc::string format_address(u64 address);
} // namespace itrace
