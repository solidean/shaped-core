#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace itrace
{
using namespace cc::primitive_defines;

struct trace;

/// Per-instruction timing from llvm-mca, aligned to one trace instruction. `valid == false` marks a
/// blank row: a trace instruction llvm-mca had no datum for (undecoded, or dropped as unparseable).
struct mca_instruction
{
    bool valid = false;

    u32 uops = 0;
    u32 latency = 0;
    double rthroughput = 0;
    bool may_load = false;
    bool may_store = false;

    /// Timeline cycles for iteration 0 (present only with -timeline). c_retired drives the "@N" suffix.
    bool has_timeline = false;
    u32 c_dispatched = 0;
    u32 c_ready = 0;
    u32 c_issued = 0;
    u32 c_executed = 0;
    u32 c_retired = 0;

    /// Usage per resource, parallel to mca_result::resources. Empty when !valid.
    cc::vector<double> port_pressure;
};

/// The SummaryView block: steady-state aggregate over `iterations` passes.
struct mca_summary
{
    double ipc = 0;
    double block_rthroughput = 0;
    double uops_per_cycle = 0;
    u64 total_cycles = 0;
    u64 total_uops = 0;
    u32 dispatch_width = 0;
    u32 iterations = 0;
};

/// One resource named as a bottleneck by -bottleneck-analysis, with its pressure cycle count.
struct mca_port_usage
{
    cc::string resource;
    double cycles = 0;
};

/// The BottleneckAnalysis block: where the modeled cycles went.
struct mca_bottleneck
{
    bool available = false;
    u64 total_cycles = 0;
    u64 data_dependency = 0;
    u64 register_dependency = 0;
    u64 memory_dependency = 0;
    u64 resource_pressure = 0;
    u64 pressure_increase = 0;
    cc::vector<mca_port_usage> top_ports;
};

/// The parsed llvm-mca analysis for one trace. `instructions` is sized to the *full* trace, aligned
/// 1:1; entries with no mca datum stay `valid == false`. `per_instruction_valid == false` means the
/// alignment could not be trusted (survivor count != mca's list) — summary + ports still hold, but
/// the per-instruction column and waterfall must be suppressed.
struct mca_result
{
    bool available = false;
    bool per_instruction_valid = false;
    cc::string cpu;
    cc::vector<cc::string> resources;
    mca_summary summary;
    cc::vector<mca_instruction> instructions;
    mca_bottleneck bottleneck;
};

/// The asm fed to llvm-mca, plus the fed-line -> trace-index map (undecoded instructions are omitted).
struct mca_input
{
    cc::string asm_text;               // ".intel_syntax noprefix\n" + one insn.text per decoded instruction
    cc::vector<u32> fed_trace_indices; // fed_trace_indices[k] = trace instruction index of fed line k
};

/// Build the llvm-mca input for a trace. Undecoded instructions (length == 0 / empty text) are skipped
/// and therefore absent from fed_trace_indices — the caller treats them as having no timing.
mca_input build_mca_input(trace const& t);

/// Parse llvm-mca's stderr and return the fed-line positions (0-based) it dropped as unparseable.
/// llvm-mca reports each drop as "<stdin>:<line>:<col>: error: ...", where line = fed position + 2
/// (line 1 is the .intel_syntax header). Duplicate reports for one line collapse to one entry.
cc::vector<u32> parse_mca_dropped_lines(cc::string_view stderr_text);

/// Parse one CodeRegion of llvm-mca -json output into the model. `surviving_trace_indices[i]` is the
/// trace instruction index that mca's Instructions[i] corresponds to (the fed set minus the dropped
/// positions, in order); `trace_instruction_count` sizes the aligned output. Pure — feed it a fixture
/// in tests. On any structural mismatch the result degrades (available/per_instruction_valid reflect
/// what could be trusted) rather than throwing.
mca_result parse_mca_json(cc::string_view json, cc::span<u32 const> surviving_trace_indices, u32 trace_instruction_count);
} // namespace itrace
