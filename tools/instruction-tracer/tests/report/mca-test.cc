#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/report/mca.hh>
#include <nexus/test.hh>

#include <cmath>

using namespace itrace;

namespace
{
// A real llvm-mca -json fixture (skylake, so it is host-independent). The input was 6 instructions;
// the 3rd (fed position 2) was unparseable and dropped, so mca's Instructions[] holds 5 survivors
// aligned to trace indices {0, 1, 3, 4, 5}.
constexpr char const* k_fixture
    = R"json({"CodeRegions":[{"BottleneckAnalysis":{"DataDependencyCycles":360,"DependencyEdge":[{"FromID":1,"ResourceOrRegID":51,"ToID":5,"Type":1},{"FromID":5,"ResourceOrRegID":51,"ToID":6,"Type":1},{"FromID":6,"ResourceOrRegID":51,"ToID":8,"Type":1}],"MemoryDependencyCycles":0,"PressureIncreaseCycles":360,"RegisterDependencyCycles":360,"ResourcePressure":[{"SKLPort1":20}],"ResourcePressureCycles":20,"TotalCycles":429},"InstructionInfoView":{"InstructionList":[{"Instruction":0,"Latency":1,"NumMicroOpcodes":1,"RThroughput":0.25,"hasUnmodeledSideEffects":false,"mayLoad":false,"mayStore":false},{"Instruction":1,"Latency":3,"NumMicroOpcodes":1,"RThroughput":1,"hasUnmodeledSideEffects":false,"mayLoad":false,"mayStore":false},{"Instruction":2,"Latency":5,"NumMicroOpcodes":1,"RThroughput":0.5,"hasUnmodeledSideEffects":false,"mayLoad":true,"mayStore":false},{"Instruction":3,"Latency":1,"NumMicroOpcodes":1,"RThroughput":0.25,"hasUnmodeledSideEffects":false,"mayLoad":false,"mayStore":false},{"Instruction":4,"Latency":7,"NumMicroOpcodes":3,"RThroughput":1,"hasUnmodeledSideEffects":true,"mayLoad":false,"mayStore":false}]},"Instructions":["add\trax, rbx","imul\trax, rcx","mov\trdx, qword ptr [rax + 16]","add\trdx, rax","ret"],"Name":"","ResourcePressureView":{"ResourcePressureInfo":[{"InstructionIndex":0,"ResourceIndex":2,"ResourceUsage":0.65},{"InstructionIndex":0,"ResourceIndex":3,"ResourceUsage":0.03},{"InstructionIndex":0,"ResourceIndex":7,"ResourceUsage":0.27},{"InstructionIndex":0,"ResourceIndex":8,"ResourceUsage":0.05},{"InstructionIndex":1,"ResourceIndex":3,"ResourceUsage":1},{"InstructionIndex":2,"ResourceIndex":4,"ResourceUsage":0.91},{"InstructionIndex":2,"ResourceIndex":5,"ResourceUsage":0.09},{"InstructionIndex":3,"ResourceIndex":2,"ResourceUsage":0.09},{"InstructionIndex":3,"ResourceIndex":3,"ResourceUsage":0.2},{"InstructionIndex":3,"ResourceIndex":7,"ResourceUsage":0.46},{"InstructionIndex":3,"ResourceIndex":8,"ResourceUsage":0.25},{"InstructionIndex":4,"ResourceIndex":2,"ResourceUsage":0.48},{"InstructionIndex":4,"ResourceIndex":3,"ResourceUsage":0.03},{"InstructionIndex":4,"ResourceIndex":4,"ResourceUsage":0.09},{"InstructionIndex":4,"ResourceIndex":5,"ResourceUsage":0.91},{"InstructionIndex":4,"ResourceIndex":7,"ResourceUsage":0.49},{"InstructionIndex":4,"ResourceIndex":8,"ResourceUsage":1},{"InstructionIndex":5,"ResourceIndex":2,"ResourceUsage":1.22},{"InstructionIndex":5,"ResourceIndex":3,"ResourceUsage":1.26},{"InstructionIndex":5,"ResourceIndex":4,"ResourceUsage":1},{"InstructionIndex":5,"ResourceIndex":5,"ResourceUsage":1},{"InstructionIndex":5,"ResourceIndex":7,"ResourceUsage":1.22},{"InstructionIndex":5,"ResourceIndex":8,"ResourceUsage":1.3}]},"SummaryView":{"BlockRThroughput":1.1666666666666667,"DispatchWidth":6,"IPC":1.1655011655011656,"Instructions":500,"Iterations":100,"TotalCycles":429,"TotaluOps":700,"uOpsPerCycle":1.6317016317016317},"TimelineView":{"TimelineInfo":[{"CycleDispatched":0,"CycleExecuted":2,"CycleIssued":1,"CycleReady":0,"CycleRetired":3},{"CycleDispatched":0,"CycleExecuted":5,"CycleIssued":2,"CycleReady":2,"CycleRetired":6},{"CycleDispatched":0,"CycleExecuted":10,"CycleIssued":5,"CycleReady":5,"CycleRetired":11},{"CycleDispatched":0,"CycleExecuted":11,"CycleIssued":10,"CycleReady":10,"CycleRetired":12},{"CycleDispatched":1,"CycleExecuted":9,"CycleIssued":2,"CycleReady":1,"CycleRetired":12}]}}],"SimulationParameters":{"-march":"x86_64","-mcpu":"skylake","-mtriple":"x86_64-pc-windows-msvc"},"TargetInfo":{"CPUName":"skylake","Resources":["SKLDivider","SKLFPDivider","SKLPort0","SKLPort1","SKLPort2","SKLPort3","SKLPort4","SKLPort5","SKLPort6","SKLPort7"]}})json";

// The exact stderr llvm-mca 22 prints for the dropped line (it reports twice; a return warning follows).
constexpr char const* k_fixture_stderr = R"stderr(<stdin>:4:1: error: invalid instruction mnemonic 'totally_bogus_insn'
totally_bogus_insn rax
^~~~~~~~~~~~~~~~~~
<stdin>:4:1: error: invalid instruction mnemonic 'totally_bogus_insn'
totally_bogus_insn rax
^~~~~~~~~~~~~~~~~~
warning: found a return instruction in the input assembly sequence.
note: program counter updates are ignored.
)stderr";

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

recorded_instruction decoded(cc::string text)
{
    recorded_instruction insn;
    insn.length = 4;
    insn.text = cc::move(text);
    return insn;
}

recorded_instruction undecoded()
{
    recorded_instruction insn;
    insn.length = 0; // not decoded: no text
    return insn;
}

// Trace instruction index -> trace index of survivors {0,1,3,4,5}, matching the fixture's dropped line.
u32 const k_survivors[] = {0, 1, 3, 4, 5};
} // namespace

TEST("mca - build_mca_input emits header and one line per decoded instruction")
{
    trace t;
    t.instructions.push_back(decoded("add rax, rbx"));
    t.instructions.push_back(undecoded()); // must be skipped
    t.instructions.push_back(decoded("ret"));

    auto const in = build_mca_input(t);
    CHECK(in.asm_text == ".intel_syntax noprefix\nadd rax, rbx\nret\n");
    REQUIRE(in.fed_trace_indices.size() == 2);
    CHECK(in.fed_trace_indices[0] == 0);
    CHECK(in.fed_trace_indices[1] == 2); // trace index 1 (undecoded) omitted
}

TEST("mca - parse_mca_dropped_lines maps stderr to fed positions, deduped")
{
    auto const dropped = parse_mca_dropped_lines(k_fixture_stderr);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0] == 2); // line 4 -> fed position 2
}

TEST("mca - parse_mca_json summary and target info")
{
    auto const r = parse_mca_json(k_fixture, k_survivors, 6);
    REQUIRE(r.available);
    CHECK(r.per_instruction_valid);
    CHECK(r.cpu == "skylake");
    REQUIRE(r.resources.size() == 10);
    CHECK(r.resources[2] == "SKLPort0");
    CHECK(r.resources[3] == "SKLPort1");

    CHECK(near(r.summary.ipc, 1.1655011655011656));
    CHECK(near(r.summary.block_rthroughput, 1.1666666666666667));
    CHECK(r.summary.dispatch_width == 6);
    CHECK(r.summary.iterations == 100);
    CHECK(r.summary.total_cycles == 429);
    CHECK(r.summary.total_uops == 700);
}

TEST("mca - parse_mca_json bottleneck")
{
    auto const r = parse_mca_json(k_fixture, k_survivors, 6);
    REQUIRE(r.bottleneck.available);
    CHECK(r.bottleneck.total_cycles == 429);
    CHECK(r.bottleneck.data_dependency == 360);
    CHECK(r.bottleneck.register_dependency == 360);
    CHECK(r.bottleneck.memory_dependency == 0);
    CHECK(r.bottleneck.resource_pressure == 20);
    REQUIRE(r.bottleneck.top_ports.size() == 1);
    CHECK(r.bottleneck.top_ports[0].resource == "SKLPort1");
    CHECK(near(r.bottleneck.top_ports[0].cycles, 20));
}

TEST("mca - parse_mca_json aligns per-instruction data to the full trace")
{
    auto const r = parse_mca_json(k_fixture, k_survivors, 6);
    REQUIRE(r.instructions.size() == 6);

    // dropped trace instruction (index 2) has no mca datum
    CHECK(!r.instructions[2].valid);

    // trace 0 = mca 0 (add): latency 1, 1 uop
    REQUIRE(r.instructions[0].valid);
    CHECK(r.instructions[0].latency == 1);
    CHECK(r.instructions[0].uops == 1);
    CHECK(near(r.instructions[0].rthroughput, 0.25));

    // trace 1 = mca 1 (imul): latency 3
    CHECK(r.instructions[1].latency == 3);

    // trace 3 = mca 2 (mov load): latency 5, mayLoad
    REQUIRE(r.instructions[3].valid);
    CHECK(r.instructions[3].latency == 5);
    CHECK(r.instructions[3].may_load);

    // trace 5 = mca 4 (ret): 3 uops, latency 7, retired at cycle 12
    REQUIRE(r.instructions[5].valid);
    CHECK(r.instructions[5].uops == 3);
    CHECK(r.instructions[5].latency == 7);
    CHECK(r.instructions[5].has_timeline);
    CHECK(r.instructions[5].c_retired == 12);
}

TEST("mca - parse_mca_json port pressure maps to resource names")
{
    auto const r = parse_mca_json(k_fixture, k_survivors, 6);
    REQUIRE(r.instructions[0].valid);
    REQUIRE(r.instructions[0].port_pressure.size() == 10);
    // mca instruction 0 used ResourceIndex 2 (SKLPort0) at 0.65 and index 3 (SKLPort1) at 0.03.
    CHECK(near(r.instructions[0].port_pressure[2], 0.65));
    CHECK(near(r.instructions[0].port_pressure[3], 0.03));
    CHECK(near(r.instructions[0].port_pressure[7], 0.27));
    // an unused port stays zero
    CHECK(near(r.instructions[0].port_pressure[0], 0.0));
}

TEST("mca - survivor mismatch degrades to summary only, never mis-attaches")
{
    // Claim only 4 survivors while the fixture has 5 -> per-instruction data must be suppressed.
    u32 const wrong[] = {0, 1, 2, 3};
    auto const r = parse_mca_json(k_fixture, wrong, 6);
    CHECK(r.available); // summary + ports still parsed
    CHECK(!r.per_instruction_valid);
    CHECK(r.summary.total_cycles == 429);
    for (auto const& mi : r.instructions)
        CHECK(!mi.valid); // no per-instruction attribution
}

TEST("mca - malformed json yields an unavailable result, not a crash")
{
    auto const r = parse_mca_json("{ this is not json", k_survivors, 6);
    CHECK(!r.available);
    CHECK(!r.per_instruction_valid);
    REQUIRE(r.instructions.size() == 6);
}
