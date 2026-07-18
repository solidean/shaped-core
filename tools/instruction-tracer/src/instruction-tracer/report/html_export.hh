#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/report/mca.hh>
#include <instruction-tracer/report/memory_formatter.hh>
#include <instruction-tracer/report/source_cache.hh>

namespace itrace
{
/// Everything main gathers after run() that is not in the trace model itself: capture-time
/// environment and the settings the run was launched with. All best-effort — an empty string just
/// renders as a blank cell.
struct html_export_meta
{
    cc::string generated_at_iso; // wall clock at export time, ISO 8601
    cc::string os_version;       // e.g. "Windows 11 Pro 10.0.26200"
    cc::string exe_path;
    u64 exe_size_bytes = 0;
    cc::string command_line; // the full argv, reconstructed
    cc::string target;       // the resolved target spec, as text

    // Capture settings, straight off `options`.
    u64 skip = 0;
    u32 traces = 0;
    u32 instructions = 0;
    bool until_return = false;
    bool stop_at_syscall = false;
    memory_view_options regions; // the default region set the header shows
};

/// Build the whole self-contained HTML page: the shell, the inlined CSS/JS assets, and one big
/// `TRACE_DATA` JSON object serialized from the traces + meta + per-trace source views. Pure over
/// already-enriched traces; `sources` is used to read the context lines for the source view.
///
/// `mca[i]` is the optional llvm-mca analysis for `traces[i]` (empty span, or an unavailable entry,
/// simply omits the timing views for that trace).
cc::string export_html(cc::span<trace const> traces,
                       html_export_meta const& meta,
                       source_cache& sources,
                       cc::span<mca_result const> mca = {});
} // namespace itrace
