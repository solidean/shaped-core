#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
class source_cache;

/// One rendered source line: its 1-based number, the untrimmed text (indentation preserved), and
/// whether any traced instruction mapped to it. (Not `source_line` — that name is a PDB file+line
/// pair in debug/symbol_session.hh.)
struct source_view_line
{
    u32 number = 0;
    cc::string text;
    bool executed = false;
};

/// A contiguous span of source lines to show together — a touched line plus its context, merged
/// with any neighbouring touched line whose context window overlaps or abuts. `start`/`end` are
/// 1-based inclusive; `lines` covers exactly [start, end].
struct source_range
{
    u32 start = 0;
    u32 end = 0;
    cc::vector<source_view_line> lines;
};

/// The touched-source view for one file: its path and the merged ranges, in ascending order.
struct source_file_view
{
    cc::string path;
    cc::vector<source_range> ranges;
};

/// Every file a trace touched, in first-appearance order, each with its merged context ranges.
struct source_view_model
{
    cc::vector<source_file_view> files;
};

/// Collect the source a trace touched: for every instruction with a source line, grow that line by
/// `context` lines each way, merge overlapping/adjacent windows into contiguous ranges per file, and
/// read the (untrimmed) text. Files whose source cannot be read are dropped. Ranges clamp to the
/// file's bounds. `context` is the ± half-window (5 → up to 11 lines around a lone touched line).
source_view_model collect_source_view(trace const& t, source_cache& sources, u32 context = 5);
} // namespace itrace
