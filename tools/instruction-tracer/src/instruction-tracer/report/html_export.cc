#include "html_export.hh"

#include <clean-core/container/map.hh>
#include <clean-core/string/format.hh>
#include <instruction-tracer/report/json_writer.hh>
#include <instruction-tracer/report/source_view.hh>
#include <instruction-tracer/report/trace_formatter.hh> // format_address
#include <instruction-tracer/report/trace_stats.hh>     // collect_stats, strip_template_args

// Generated at build time from report/html/{app.css,app.js} by embed-html-assets.py. Provides
// itrace::html::app_css and itrace::html::app_js as inline constexpr raw-string literals.
#include <html_assets.hh>

namespace itrace
{
namespace
{
cc::string_view reason_text(step_reason reason)
{
    switch (reason)
    {
    case step_reason::instruction_budget:
        return "instruction budget reached (--instructions)";
    case step_reason::returned:
        return "original stack frame returned";
    case step_reason::syscall:
        return "syscall boundary (stopped before entering the kernel)";
    case step_reason::exception:
        return "the debuggee raised an exception";
    case step_reason::process_exited:
        return "the debuggee exited mid-trace";
    }
    return "unknown";
}

cc::string_view category_name(insn_category c)
{
    switch (c)
    {
    case insn_category::other:
        return "other";
    case insn_category::conditional_branch:
        return "conditional_branch";
    case insn_category::unconditional_branch:
        return "unconditional_branch";
    case insn_category::call:
        return "call";
    case insn_category::ret:
        return "ret";
    case insn_category::syscall:
        return "syscall";
    }
    return "other";
}

cc::string_view region_name(access_region r)
{
    switch (r)
    {
    case access_region::heap:
        return "heap";
    case access_region::frame:
        return "frame";
    case access_region::stack:
        return "stack";
    case access_region::instructions:
        return "instructions";
    }
    return "heap";
}

// The status flags the trace reports, mirroring trace_formatter.cc. TF/IF/RF are excluded there for
// the same reasons (TF is the tracer's own single-step bit).
struct flag_bit
{
    u32 bit;
    char const* name;
};
constexpr flag_bit flag_bits[] = {
    {0, "CF"}, {2, "PF"}, {4, "AF"}, {6, "ZF"}, {7, "SF"}, {10, "DF"}, {11, "OF"},
};
bool flag_set(u64 rflags, u32 bit)
{
    return ((rflags >> bit) & 1) != 0;
}

/// The first whitespace-delimited token of the disassembly, i.e. the mnemonic. Empty for an
/// undecoded instruction (whose `text` is empty).
cc::string_view mnemonic_of(cc::string_view text)
{
    auto const space = text.find(' ');
    return space < 0 ? text : text.subview({.start = 0, .end = space});
}

/// The disassembly to display: the decoded text, or the raw bytes in parentheses when the decoder
/// could not read the instruction (matching the terminal's fallback).
cc::string display_text(recorded_instruction const& insn)
{
    if (!insn.text.empty())
        return insn.text;

    auto const n = insn.length > 0 ? insn.length : insn.byte_count;
    cc::string out = "(";
    for (u8 i = 0; i < n; ++i)
        cc::format_append(out, "{}{:02x}", i == 0 ? "" : " ", insn.bytes[isize(i)]);
    out += ")";
    return out;
}

/// {name, value} pairs for the GPRs and flags this instruction changed — the same set the terminal's
/// register-diff shows. Values are strings (a register can hold a full 64-bit pointer).
void write_regdiff(json_writer& j, register_snapshot const& before, register_snapshot const& after)
{
    j.begin_array();
    for (int i = 0; i < gpr_count; ++i)
    {
        if (before.gpr[i] == after.gpr[i])
            continue;
        j.begin_object();
        j.field("name", gpr_names[i]);
        j.field("value", cc::format("{:#x}", after.gpr[i]));
        j.end_object();
    }
    for (auto const& f : flag_bits)
    {
        if (flag_set(before.rflags, f.bit) == flag_set(after.rflags, f.bit))
            continue;
        j.begin_object();
        j.field("name", f.name);
        j.field("value", flag_set(after.rflags, f.bit) ? "1" : "0");
        j.end_object();
    }
    j.end_array();
}

/// The full entry state: every GPR, rflags, and the named status flags currently set.
void write_registers(json_writer& j, register_snapshot const& s)
{
    j.begin_object();
    j.key("gpr");
    j.begin_array();
    for (int i = 0; i < gpr_count; ++i)
    {
        j.begin_object();
        j.field("name", gpr_names[i]);
        j.field("value", cc::format("{:#018x}", s.gpr[i]));
        j.end_object();
    }
    j.end_array();
    j.field("rflags", cc::format("{:#010x}", s.rflags));
    j.key("flags");
    j.begin_array();
    for (auto const& f : flag_bits)
        if (flag_set(s.rflags, f.bit))
            j.value_string(f.name);
    j.end_array();
    j.end_object();
}

void write_meta(json_writer& j, html_export_meta const& meta)
{
    j.key("meta");
    j.begin_object();
    j.field("generatedAt", meta.generated_at_iso);
    j.field("osVersion", meta.os_version);
    j.field("exePath", meta.exe_path);
    j.field("exeSizeBytes", cc::format("{}", meta.exe_size_bytes));
    j.field("commandLine", meta.command_line);
    j.field("target", meta.target);
    j.field_uint("skip", meta.skip);
    j.field_uint("traces", meta.traces);
    j.field_uint("instructions", meta.instructions);
    j.field_bool("untilReturn", meta.until_return);
    j.field_bool("stopAtSyscall", meta.stop_at_syscall);
    j.key("regions");
    j.begin_object();
    j.field_bool("heap", meta.regions.heap);
    j.field_bool("frame", meta.regions.frame);
    j.field_bool("stack", meta.regions.stack);
    j.field_bool("instructions", meta.regions.instructions);
    j.end_object();
    j.end_object();
}

void write_stats(json_writer& j, trace const& t)
{
    // Per-trace table: a one-element span, unlike the CLI's global aggregate.
    auto const summary = collect_stats(cc::span<trace const>(&t, 1));

    j.key("stats");
    j.begin_object();
    j.key("rows");
    j.begin_array();
    for (auto const& r : summary.rows)
    {
        j.begin_object();
        j.field("symbol", r.symbol);
        j.field_uint("instructions", r.instructions);
        j.field_uint("atomics", r.atomics);
        j.field_uint("slow", r.slow);
        j.field_uint("directCalls", r.direct_calls);
        j.field_uint("indirectCalls", r.indirect_calls);
        j.field_uint("memoryReads", r.memory_reads);
        j.field_uint("memoryWrites", r.memory_writes);
        j.field_uint("branches", r.branches);
        j.field_uint("branchesTaken", r.branches_taken);
        j.end_object();
    }
    j.end_array();
    j.key("slowOps");
    j.begin_array();
    for (auto const& s : summary.slow_ops)
    {
        j.begin_object();
        j.field("mnemonic", s.mnemonic);
        j.field("symbol", s.symbol);
        j.field_uint("count", s.count);
        j.end_object();
    }
    j.end_array();
    j.field_uint("traces", summary.traces);
    j.field_bool("truncated", summary.truncated);
    j.end_object();
}

void write_source(json_writer& j, source_view_model const& sv, cc::map<cc::string, int> const& file_ids)
{
    j.key("source");
    j.begin_object();
    j.key("files");
    j.begin_array();
    for (auto const& f : sv.files)
    {
        j.begin_object();
        j.field_int("fileId", file_ids.get_or(f.path, -1));
        j.field("path", f.path);
        // Display name: the last path component, for the range sub-headers.
        auto slash = f.path.size();
        for (isize i = f.path.size() - 1; i >= 0; --i)
            if (f.path[i] == '\\' || f.path[i] == '/')
            {
                slash = i + 1;
                break;
            }
        j.field("displayName", f.path.subview({.start = slash, .end = f.path.size()}));
        j.key("ranges");
        j.begin_array();
        for (auto const& range : f.ranges)
        {
            j.begin_object();
            j.field_uint("start", range.start);
            j.field_uint("end", range.end);
            j.key("lines");
            j.begin_array();
            for (auto const& line : range.lines)
            {
                j.begin_object();
                j.field_uint("number", line.number);
                j.field("text", line.text);
                j.field_bool("executed", line.executed);
                j.end_object();
            }
            j.end_array();
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();
    j.end_object();
}

void write_trace(json_writer& j, trace const& t, u32 total, source_cache& sources)
{
    // A per-trace file-path → id map, shared between instructions and the source view so the
    // front-end can cross-highlight a source line and the instructions that ran it.
    cc::map<cc::string, int> file_ids;
    int next_id = 0;
    for (auto const& insn : t.instructions)
        if (!insn.file.empty())
        {
            auto e = file_ids.entry(insn.file);
            if (!e.exists())
                e.emplace(next_id++);
        }

    j.begin_object();
    j.field_uint("index", t.index);
    j.field_uint("total", total);
    j.field_uint("threadId", t.thread_id);
    j.field_uint("hit", t.hit_index);
    j.field("entrySymbol", t.entry_symbol);
    j.field("returnSymbol", t.return_symbol);
    j.field("reason", reason_text(t.reason));
    j.field_uint("instructionCount", u32(t.instructions.size()));
    j.field_bool("truncated", t.reason == step_reason::instruction_budget);

    j.key("stack");
    j.begin_array();
    for (auto const& f : t.entry_stack)
    {
        j.begin_object();
        j.field("symbol", f.symbol);
        j.field("module", f.module);
        j.field("file", f.file);
        j.field_uint("line", f.line);
        j.field("addr", format_address(f.rip));
        j.end_object();
    }
    j.end_array();

    // The trailing snapshot is absent at the syscall stop; the front snapshot is the entry state.
    if (!t.registers.empty())
    {
        j.key("entryRegisters");
        write_registers(j, t.registers.front());
    }
    else
    {
        j.key("entryRegisters");
        j.value_null();
    }

    j.key("instructions");
    j.begin_array();
    for (isize i = 0; i < t.instructions.size(); ++i)
    {
        auto const& insn = t.instructions[i];
        j.begin_object();
        j.field("addr", format_address(insn.rip));
        j.field("text", display_text(insn));
        auto const m = mnemonic_of(insn.text);
        if (m.empty())
        {
            j.key("mnemonic");
            j.value_null();
        }
        else
            j.field("mnemonic", m);
        j.field_int("fileId", insn.file.empty() ? -1 : file_ids.get_or(insn.file, -1));
        j.field("file", insn.file);
        j.field_uint("line", insn.line);
        j.field("category", category_name(insn.category));
        j.field_bool("isAtomic", insn.is_atomic);
        if (insn.slow_mnemonic != nullptr)
            j.field("slowMnemonic", insn.slow_mnemonic);
        else
        {
            j.key("slowMnemonic");
            j.value_null();
        }
        j.field_bool("isIndirect", insn.is_indirect);
        j.field_bool("diverged", diverged(insn));
        j.field_bool("branchTaken", insn.category == insn_category::conditional_branch && diverged(insn));
        if (insn.target_symbol.empty())
        {
            j.key("target");
            j.value_null();
        }
        else
            j.field("target", insn.target_symbol);
        j.field("owner", insn.owner_symbol.empty() ? cc::string() : strip_template_args(insn.owner_symbol));

        j.key("regdiff");
        if (i + 1 < t.registers.size())
            write_regdiff(j, t.registers[i], t.registers[i + 1]);
        else
            j.begin_array(), j.end_array();

        j.key("mem");
        j.begin_array();
        for (auto const& acc : insn.memory_accesses)
        {
            j.begin_object();
            j.field("addr", format_address(acc.address));
            j.field_uint("size", acc.size);
            j.field_bool("isRead", acc.is_read);
            j.field_bool("isWrite", acc.is_write);
            j.field("region", region_name(acc.region));
            j.field("symbol", acc.symbol);
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();

    write_stats(j, t);
    write_source(j, collect_source_view(t, sources), file_ids);

    j.end_object();
}

cc::string serialize(cc::span<trace const> traces, html_export_meta const& meta, source_cache& sources)
{
    json_writer j;
    j.begin_object();
    write_meta(j, meta);
    j.key("traces");
    j.begin_array();
    auto const total = u32(traces.size());
    for (auto const& t : traces)
        write_trace(j, t, total, sources);
    j.end_array();
    j.end_object();
    return j.str();
}
} // namespace

cc::string export_html(cc::span<trace const> traces, html_export_meta const& meta, source_cache& sources)
{
    cc::string title = meta.target.empty() ? cc::string("instruction trace") : meta.target;

    cc::string out;
    out += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>trace: " + title + "</title>\n";
    out += "<style>\n";
    out += html::app_css;
    out += "\n</style>\n</head>\n<body>\n";
    out += "<div id=\"app\"></div>\n";
    out += "<script>\nconst TRACE_DATA = ";
    out += serialize(traces, meta, sources);
    out += ";\n</script>\n";
    out += "<script>\n";
    out += html::app_js;
    out += "\n</script>\n</body></html>\n";
    return out;
}
} // namespace itrace
