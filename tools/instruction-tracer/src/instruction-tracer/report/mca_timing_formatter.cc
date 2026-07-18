#include "mca_timing_formatter.hh"

#include <clean-core/string/format.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/report/console.hh>
#include <instruction-tracer/report/mca.hh>
#include <instruction-tracer/report/trace_formatter.hh>

namespace itrace
{
namespace
{
void append_summary(cc::string& out, mca_summary const& s)
{
    out += "  ";
    out += cc::format("IPC {:.2f}   block RThroughput {:.2f}   cycles {}   uops {}   dispatch {}   iters {}", s.ipc,
                      s.block_rthroughput, s.total_cycles, s.total_uops, s.dispatch_width, s.iterations);
    out += "\n";
}

void append_bottleneck(cc::string& out, mca_bottleneck const& b)
{
    if (!b.available)
        return;
    out += "  ";
    out += dim("bottleneck: ");
    out += cc::format("register-dep {}c  data-dep {}c  memory-dep {}c  resource {}c", b.register_dependency,
                      b.data_dependency, b.memory_dependency, b.resource_pressure);
    if (!b.top_ports.empty())
    {
        out += "  ";
        cc::string ports;
        for (auto const& p : b.top_ports)
        {
            if (!ports.empty())
                ports += ", ";
            ports += cc::format("{} ({:.0f}c)", p.resource, p.cycles);
        }
        out += yellow(cc::format("limited by {}", ports));
    }
    out += "\n";
}

void append_instructions(cc::string& out, trace const& t, mca_result const& r)
{
    out += dim("  addr              uops  lat   @ret  text") + "\n";
    for (isize i = 0; i < t.instructions.size() && i < r.instructions.size(); ++i)
    {
        auto const& mi = r.instructions[i];
        if (!mi.valid)
            continue;
        auto const& insn = t.instructions[i];
        cc::string line;
        line += "  ";
        line += dim(format_address(insn.rip));
        line += cc::format("  {:>4}  {:>3}  ", mi.uops, mi.latency);
        line += mi.has_timeline ? cc::format("{:>5}", cc::format("@{}", mi.c_retired)) : cc::string("     ");
        line += "  ";
        line += insn.text.empty() ? cc::string("(undecoded)") : insn.text;
        line += "\n";
        out += line;
    }
}
} // namespace

cc::string format_mca_timing(cc::span<trace const> traces, cc::span<mca_result const> mca)
{
    bool any_available = false;
    for (auto const& r : mca)
        if (r.available)
            any_available = true;

    if (!any_available)
        return dim("timing unavailable: llvm-mca did not run (pass --mca <path>)") + "\n";

    cc::string out;
    auto const total = u32(traces.size());
    for (isize i = 0; i < traces.size(); ++i)
    {
        auto const& t = traces[i];
        mca_result const empty;
        mca_result const& r = i < mca.size() ? mca[i] : empty;

        if (i > 0)
            out += "\n";
        out += bold(cc::format("timing  trace {}/{}", t.index, total));
        if (!r.cpu.empty())
            out += dim(cc::format("   model {}", r.cpu));
        out += "\n";

        if (!r.available)
        {
            out += dim("  (llvm-mca produced no analysis for this trace)") + "\n";
            continue;
        }

        append_summary(out, r.summary);
        append_bottleneck(out, r.bottleneck);

        if (r.per_instruction_valid)
            append_instructions(out, t, r);
        else
            out += dim("  (per-instruction timing unavailable: the stream did not reconcile with llvm-mca)") + "\n";
    }
    return out;
}
} // namespace itrace
