#include "mca.hh"

#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/report/json_reader.hh>

namespace itrace
{
namespace
{
double num_field(json_value const* obj, cc::string_view key)
{
    auto const* v = obj->find(key);
    return v ? v->as_number() : 0.0;
}

bool bool_field(json_value const* obj, cc::string_view key)
{
    auto const* v = obj->find(key);
    return v ? v->as_bool() : false;
}
} // namespace

mca_input build_mca_input(trace const& t)
{
    mca_input in;
    in.asm_text = ".intel_syntax noprefix\n";
    for (u32 i = 0; i < t.instructions.size(); ++i)
    {
        auto const& insn = t.instructions[i];
        if (insn.length == 0 || insn.text.empty()) // undecoded: no text to feed, treated as dropped
            continue;
        in.asm_text += insn.text;
        in.asm_text += '\n';
        in.fed_trace_indices.push_back(i);
    }
    return in;
}

cc::vector<u32> parse_mca_dropped_lines(cc::string_view stderr_text)
{
    cc::vector<u32> dropped;
    cc::string_view const prefix = "<stdin>:";
    cc::string_view const marker = ": error:";

    isize start = 0;
    while (start <= stderr_text.size())
    {
        auto const nl = stderr_text.find('\n', start);
        auto const line_end = nl < 0 ? stderr_text.size() : nl;
        cc::string_view const line = stderr_text.subview({.start = start, .end = line_end});

        if (line.starts_with(prefix) && line.contains(marker))
        {
            cc::string_view const rest = line.subview(prefix.size());
            u32 line_no = 0;
            isize k = 0;
            bool any = false;
            while (k < rest.size() && rest[k] >= '0' && rest[k] <= '9')
            {
                line_no = line_no * 10 + u32(rest[k] - '0');
                ++k;
                any = true;
            }
            if (any && line_no >= 2) // line 1 is the .intel_syntax header
            {
                u32 const pos = line_no - 2;
                bool seen = false;
                for (u32 const d : dropped)
                    if (d == pos)
                        seen = true;
                if (!seen)
                    dropped.push_back(pos);
            }
        }

        if (nl < 0)
            break;
        start = nl + 1;
    }
    return dropped;
}

mca_result parse_mca_json(cc::string_view json, cc::span<u32 const> surviving_trace_indices, u32 trace_instruction_count)
{
    mca_result result;
    result.instructions.resize_to_defaulted(trace_instruction_count); // all blank until filled

    auto parsed = parse_json(json);
    if (parsed.has_error())
        return result; // available stays false

    json_value const& root = parsed.value();

    if (auto const* target = root.find("TargetInfo"))
    {
        if (auto const* cpu = target->find("CPUName"))
            result.cpu = cpu->as_string();
        if (auto const* res = target->find("Resources"); res && res->is_array())
            for (isize i = 0; i < res->size(); ++i)
                result.resources.push_back(cc::string(res->at(i)->as_string()));
    }

    auto const* regions = root.find("CodeRegions");
    if (!regions || !regions->is_array() || regions->size() == 0)
        return result;
    json_value const& region = *regions->at(0);
    result.available = true;

    if (auto const* sv = region.find("SummaryView"))
    {
        auto& s = result.summary;
        s.ipc = num_field(sv, "IPC");
        s.block_rthroughput = num_field(sv, "BlockRThroughput");
        s.uops_per_cycle = num_field(sv, "uOpsPerCycle");
        s.total_cycles = u64(num_field(sv, "TotalCycles"));
        s.total_uops = u64(num_field(sv, "TotaluOps"));
        s.dispatch_width = u32(num_field(sv, "DispatchWidth"));
        s.iterations = u32(num_field(sv, "Iterations"));
    }

    if (auto const* ba = region.find("BottleneckAnalysis"))
    {
        auto& b = result.bottleneck;
        b.available = true;
        b.total_cycles = u64(num_field(ba, "TotalCycles"));
        b.data_dependency = u64(num_field(ba, "DataDependencyCycles"));
        b.register_dependency = u64(num_field(ba, "RegisterDependencyCycles"));
        b.memory_dependency = u64(num_field(ba, "MemoryDependencyCycles"));
        b.resource_pressure = u64(num_field(ba, "ResourcePressureCycles"));
        b.pressure_increase = u64(num_field(ba, "PressureIncreaseCycles"));
        if (auto const* rp = ba->find("ResourcePressure"); rp && rp->is_array())
            for (isize i = 0; i < rp->size(); ++i)
            {
                json_value const* entry = rp->at(i);
                if (entry->is_object() && entry->size() > 0) // single-key object {resource: cycles}
                {
                    auto const& kv = entry->obj[0];
                    b.top_ports.push_back({kv.first, kv.second.as_number()});
                }
            }
    }

    // Per-instruction data — only trustworthy when mca's survivor list matches our alignment.
    auto const* instr_echo = region.find("Instructions");
    isize const survivor_count = (instr_echo && instr_echo->is_array()) ? instr_echo->size() : -1;
    if (survivor_count < 0 || survivor_count != surviving_trace_indices.size())
        return result; // degrade: per_instruction_valid stays false, summary + ports still hold

    cc::vector<mca_instruction> rows;
    rows.resize_to_defaulted(survivor_count);
    for (auto& r : rows)
    {
        r.valid = true;
        r.port_pressure.resize_to_defaulted(result.resources.size());
    }

    if (auto const* iiv = region.find("InstructionInfoView"))
        if (auto const* list = iiv->find("InstructionList"); list && list->is_array())
            for (isize i = 0; i < list->size() && i < survivor_count; ++i)
            {
                json_value const* e = list->at(i);
                rows[i].uops = u32(num_field(e, "NumMicroOpcodes"));
                rows[i].latency = u32(num_field(e, "Latency"));
                rows[i].rthroughput = num_field(e, "RThroughput");
                rows[i].may_load = bool_field(e, "mayLoad");
                rows[i].may_store = bool_field(e, "mayStore");
            }

    if (auto const* tv = region.find("TimelineView"))
        if (auto const* ti = tv->find("TimelineInfo"); ti && ti->is_array())
            for (isize i = 0; i < ti->size() && i < survivor_count; ++i)
            {
                json_value const* e = ti->at(i);
                rows[i].has_timeline = true;
                rows[i].c_dispatched = u32(num_field(e, "CycleDispatched"));
                rows[i].c_ready = u32(num_field(e, "CycleReady"));
                rows[i].c_issued = u32(num_field(e, "CycleIssued"));
                rows[i].c_executed = u32(num_field(e, "CycleExecuted"));
                rows[i].c_retired = u32(num_field(e, "CycleRetired"));
            }

    if (auto const* rpv = region.find("ResourcePressureView"))
        if (auto const* rpi = rpv->find("ResourcePressureInfo"); rpi && rpi->is_array())
            for (isize i = 0; i < rpi->size(); ++i)
            {
                json_value const* e = rpi->at(i);
                isize const ii = isize(num_field(e, "InstructionIndex"));
                isize const ri = isize(num_field(e, "ResourceIndex"));
                // ii == survivor_count is the per-resource totals row; only per-instruction rows here.
                if (ii >= 0 && ii < survivor_count && ri >= 0 && ri < result.resources.size())
                    rows[ii].port_pressure[ri] = num_field(e, "ResourceUsage");
            }

    for (isize i = 0; i < survivor_count; ++i)
    {
        u32 const ti = surviving_trace_indices[i];
        if (ti < trace_instruction_count)
            result.instructions[ti] = cc::move(rows[i]);
    }
    result.per_instruction_valid = true;
    return result;
}
} // namespace itrace
