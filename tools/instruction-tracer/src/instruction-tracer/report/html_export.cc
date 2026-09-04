#include "html_export.hh"

#include <babel-data/data/json.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/format.hh>
#include <instruction-tracer/report/source_view.hh>
#include <instruction-tracer/report/trace_formatter.hh> // format_address
#include <instruction-tracer/report/trace_stats.hh>     // collect_stats, strip_template_args

// Generated at build time from report/html/{app.css,app.js} by embed-html-assets.py.
// Provides itrace::html::app_css and itrace::html::app_js as inline constexpr raw-string literals.
#include <html_assets.hh>

namespace itrace
{
namespace
{
namespace json = babel::json;

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

// The status flags the trace reports, mirroring trace_formatter.cc — which is where TF/IF/RF are excluded, and why.
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

/// The first whitespace-delimited token of the disassembly, i.e. the mnemonic.
/// Empty for an undecoded instruction, whose `text` is empty.
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

/// {name, value} pairs for the GPRs and flags this instruction changed — the same set the terminal's register-diff shows.
/// Values are strings, since a register can hold a full 64-bit pointer.
void write_regdiff(json::object_writer& parent,
                   cc::string_view key,
                   register_snapshot const& before,
                   register_snapshot const& after)
{
    auto diff = parent.write_array(key);
    for (int i = 0; i < gpr_count; ++i)
    {
        if (before.gpr[i] == after.gpr[i])
            continue;
        auto r = diff.write_object();
        r.write("name", gpr_names[i]);
        r.write("value", cc::format("{:#x}", after.gpr[i]));
    }
    for (auto const& f : flag_bits)
    {
        if (flag_set(before.rflags, f.bit) == flag_set(after.rflags, f.bit))
            continue;
        auto r = diff.write_object();
        r.write("name", f.name);
        r.write("value", flag_set(after.rflags, f.bit) ? "1" : "0");
    }
}

/// The full entry state: every GPR, rflags, and the named status flags currently set.
void write_registers(json::object_writer& parent, cc::string_view key, register_snapshot const& s)
{
    auto regs = parent.write_object(key);
    {
        auto gpr = regs.write_array("gpr");
        for (int i = 0; i < gpr_count; ++i)
        {
            auto r = gpr.write_object();
            r.write("name", gpr_names[i]);
            r.write("value", cc::format("{:#018x}", s.gpr[i]));
        }
    }
    regs.write("rflags", cc::format("{:#010x}", s.rflags));
    {
        auto flags = regs.write_array("flags");
        for (auto const& f : flag_bits)
            if (flag_set(s.rflags, f.bit))
                flags.write(f.name);
    }
}

void write_meta(json::object_writer& root, html_export_meta const& meta)
{
    auto m = root.write_object("meta");
    m.write("generatedAt", meta.generated_at_iso);
    m.write("osVersion", meta.os_version);
    m.write("exePath", meta.exe_path);
    m.write("exeSizeBytes", cc::format("{}", meta.exe_size_bytes));
    m.write("commandLine", meta.command_line);
    m.write("target", meta.target);
    m.write("skip", meta.skip);
    m.write("traces", meta.traces);
    m.write("instructions", meta.instructions);
    m.write("untilReturn", meta.until_return);
    m.write("stopAtSyscall", meta.stop_at_syscall);

    auto regions = m.write_object("regions");
    regions.write("heap", meta.regions.heap);
    regions.write("frame", meta.regions.frame);
    regions.write("stack", meta.regions.stack);
    regions.write("instructions", meta.regions.instructions);
}

void write_stats(json::object_writer& out, trace const& t)
{
    // Per-trace table: a one-element span, unlike the CLI's global aggregate.
    auto const summary = collect_stats(cc::span<trace const>(&t, 1));

    auto stats = out.write_object("stats");
    {
        auto rows = stats.write_array("rows");
        for (auto const& r : summary.rows)
        {
            auto row = rows.write_object();
            row.write("symbol", r.symbol);
            row.write("instructions", r.instructions);
            row.write("atomics", r.atomics);
            row.write("slow", r.slow);
            row.write("directCalls", r.direct_calls);
            row.write("indirectCalls", r.indirect_calls);
            row.write("memoryReads", r.memory_reads);
            row.write("memoryWrites", r.memory_writes);
            row.write("branches", r.branches);
            row.write("branchesTaken", r.branches_taken);
        }
    }
    {
        auto slow_ops = stats.write_array("slowOps");
        for (auto const& s : summary.slow_ops)
        {
            auto op = slow_ops.write_object();
            op.write("mnemonic", s.mnemonic);
            op.write("symbol", s.symbol);
            op.write("count", s.count);
        }
    }
    stats.write("traces", summary.traces);
    stats.write("truncated", summary.truncated);
}

void write_source(json::object_writer& out, source_view_model const& sv, cc::map<cc::string, int> const& file_ids)
{
    auto source = out.write_object("source");
    auto files = source.write_array("files");
    for (auto const& f : sv.files)
    {
        auto file = files.write_object();
        file.write("fileId", file_ids.get_or(f.path, -1));
        file.write("path", f.path);

        // Display name: the last path component, for the range sub-headers.
        auto slash = f.path.size();
        for (isize i = f.path.size() - 1; i >= 0; --i)
            if (f.path[i] == '\\' || f.path[i] == '/')
            {
                slash = i + 1;
                break;
            }
        file.write("displayName", f.path.subview({.start = slash, .end = f.path.size()}));

        auto ranges = file.write_array("ranges");
        for (auto const& range : f.ranges)
        {
            auto r = ranges.write_object();
            r.write("start", range.start);
            r.write("end", range.end);

            auto lines = r.write_array("lines");
            for (auto const& line : range.lines)
            {
                auto l = lines.write_object();
                l.write("number", line.number);
                l.write("text", line.text);
                l.write("executed", line.executed);
            }
        }
    }
}

void write_mca(json::object_writer& out, mca_result const& m)
{
    auto mca = out.write_object("mca");
    mca.write("available", m.available);
    mca.write("perInstructionValid", m.per_instruction_valid);
    mca.write("cpu", m.cpu);

    {
        auto resources = mca.write_array("resources");
        for (auto const& r : m.resources)
            resources.write(r);
    }
    {
        auto summary = mca.write_object("summary");
        summary.write("ipc", m.summary.ipc);
        summary.write("blockRThroughput", m.summary.block_rthroughput);
        summary.write("uopsPerCycle", m.summary.uops_per_cycle);
        summary.write("totalCycles", m.summary.total_cycles);
        summary.write("totalUops", m.summary.total_uops);
        summary.write("dispatchWidth", m.summary.dispatch_width);
        summary.write("iterations", m.summary.iterations);
    }
    {
        auto bottleneck = mca.write_object("bottleneck");
        bottleneck.write("available", m.bottleneck.available);
        bottleneck.write("totalCycles", m.bottleneck.total_cycles);
        bottleneck.write("dataDependency", m.bottleneck.data_dependency);
        bottleneck.write("registerDependency", m.bottleneck.register_dependency);
        bottleneck.write("memoryDependency", m.bottleneck.memory_dependency);
        bottleneck.write("resourcePressure", m.bottleneck.resource_pressure);
        bottleneck.write("pressureIncrease", m.bottleneck.pressure_increase);

        auto top_ports = bottleneck.write_array("topPorts");
        for (auto const& p : m.bottleneck.top_ports)
        {
            auto port = top_ports.write_object();
            port.write("resource", p.resource);
            port.write("cycles", p.cycles);
        }
    }

    // Aligned 1:1 to the trace instructions; a blank {valid:false} keeps the index mapping.
    auto instructions = mca.write_array("instructions");
    for (auto const& mi : m.instructions)
    {
        auto insn = instructions.write_object();
        insn.write("valid", mi.valid);
        if (!mi.valid)
            continue;

        insn.write("uops", mi.uops);
        insn.write("latency", mi.latency);
        insn.write("rthroughput", mi.rthroughput);
        insn.write("mayLoad", mi.may_load);
        insn.write("mayStore", mi.may_store);
        insn.write("hasTimeline", mi.has_timeline);
        insn.write("cDispatched", mi.c_dispatched);
        insn.write("cReady", mi.c_ready);
        insn.write("cIssued", mi.c_issued);
        insn.write("cExecuted", mi.c_executed);
        insn.write("cRetired", mi.c_retired);

        auto pressure = insn.write_array("portPressure");
        for (double const usage : mi.port_pressure)
            pressure.write(usage);
    }
}

void write_trace(json::array_writer& out, trace const& t, u32 total, source_cache& sources, mca_result const* mca)
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

    auto tr = out.write_object();
    tr.write("index", t.index);
    tr.write("total", total);
    tr.write("threadId", t.thread_id);
    tr.write("hit", t.hit_index);
    tr.write("entrySymbol", t.entry_symbol);
    tr.write("returnSymbol", t.return_symbol);
    tr.write("reason", reason_text(t.reason));
    tr.write("instructionCount", u32(t.instructions.size()));
    tr.write("truncated", t.reason == step_reason::instruction_budget);

    {
        auto stack = tr.write_array("stack");
        for (auto const& f : t.entry_stack)
        {
            auto frame = stack.write_object();
            frame.write("symbol", f.symbol);
            frame.write("module", f.module);
            frame.write("file", f.file);
            frame.write("line", f.line);
            frame.write("addr", format_address(f.rip));
        }
    }

    // The trailing snapshot is absent at the syscall stop; the front snapshot is the entry state.
    if (!t.registers.empty())
        write_registers(tr, "entryRegisters", t.registers.front());
    else
        tr.write("entryRegisters", nullptr);

    {
        auto instructions = tr.write_array("instructions");
        for (isize i = 0; i < t.instructions.size(); ++i)
        {
            auto const& insn = t.instructions[i];

            auto o = instructions.write_object();
            o.write("addr", format_address(insn.rip));
            o.write("text", display_text(insn));

            auto const m = mnemonic_of(insn.text);
            if (m.empty())
                o.write("mnemonic", nullptr);
            else
                o.write("mnemonic", m);

            o.write("fileId", insn.file.empty() ? -1 : file_ids.get_or(insn.file, -1));
            o.write("file", insn.file);
            o.write("line", insn.line);
            o.write("category", category_name(insn.category));
            o.write("isAtomic", insn.is_atomic);

            if (insn.slow_mnemonic != nullptr)
                o.write("slowMnemonic", cc::string_view(insn.slow_mnemonic));
            else
                o.write("slowMnemonic", nullptr);

            o.write("isIndirect", insn.is_indirect);
            o.write("diverged", diverged(insn));
            o.write("branchTaken", insn.category == insn_category::conditional_branch && diverged(insn));

            if (insn.target_symbol.empty())
                o.write("target", nullptr);
            else
                o.write("target", insn.target_symbol);

            o.write("owner", insn.owner_symbol.empty() ? cc::string() : strip_template_args(insn.owner_symbol));

            if (i + 1 < t.registers.size())
                write_regdiff(o, "regdiff", t.registers[i], t.registers[i + 1]);
            else
                (void)o.write_array("regdiff"); // the last instruction has no next snapshot to diff against

            auto mem = o.write_array("mem");
            for (auto const& acc : insn.memory_accesses)
            {
                auto a = mem.write_object();
                a.write("addr", format_address(acc.address));
                a.write("size", acc.size);
                a.write("isRead", acc.is_read);
                a.write("isWrite", acc.is_write);
                a.write("region", region_name(acc.region));
                a.write("symbol", acc.symbol);
            }
        }
    }

    write_stats(tr, t);
    write_source(tr, collect_source_view(t, sources), file_ids);

    if (mca != nullptr && mca->available)
        write_mca(tr, *mca);
}

cc::string serialize(cc::span<trace const> traces,
                     html_export_meta const& meta,
                     source_cache& sources,
                     cc::span<mca_result const> mca)
{
    // escape_html: the payload is embedded in a <script> tag, so a symbol or source line containing "</script>"
    //   must not be able to end it.
    //   The JS parser turns the escape back into '<', so the data is unchanged.
    // non_finite -> null: an MCA number that came out NaN is a hole in one table, not a reason to lose the report.
    auto w = babel::json::string_writer({
        .non_finite = babel::json::non_finite_policy::null,
        .escape_html = true,
    });

    {
        auto root = w.object();
        write_meta(root, meta);

        auto all = root.write_array("traces");
        auto const total = u32(traces.size());
        for (isize i = 0; i < traces.size(); ++i)
            write_trace(all, traces[i], total, sources, i < mca.size() ? &mca[i] : nullptr);
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
} // namespace

cc::string export_html(cc::span<trace const> traces,
                       html_export_meta const& meta,
                       source_cache& sources,
                       cc::span<mca_result const> mca)
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
    out += serialize(traces, meta, sources, mca);
    out += ";\n</script>\n";
    out += "<script>\n";
    out += html::app_js;
    out += "\n</script>\n</body></html>\n";
    return out;
}
} // namespace itrace
