#include <instruction-tracer/report/memory_formatter.hh>
#include <instruction-tracer/report/memory_stats.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
memory_access access(u64 address, u16 size, bool read, bool write, access_region region, cc::string symbol = {})
{
    memory_access a;
    a.address = address;
    a.size = size;
    a.is_read = read;
    a.is_write = write;
    a.region = region;
    a.symbol = cc::move(symbol);
    return a;
}

recorded_instruction insn_with(u64 rip, cc::string owner, cc::vector<memory_access> accesses)
{
    recorded_instruction insn;
    insn.rip = rip;
    insn.owner_symbol = cc::move(owner);
    insn.memory_accesses = cc::move(accesses);
    return insn;
}

trace one_trace(cc::vector<recorded_instruction> instructions)
{
    trace t;
    t.index = 1;
    t.instructions = cc::move(instructions);
    return t;
}
} // namespace

TEST("memory raw - lists included regions and names symbols")
{
    auto t = one_trace({insn_with(0x1000, "mod!foo",
                                  {access(0x4000, 8, true, false, access_region::heap, "mod!g_global"),
                                   access(0x7ff0, 8, false, true, access_region::frame)})});
    trace const traces[] = {cc::move(t)};

    memory_view_options opts; // default: heap + stack
    auto const out = format_memory_raw(traces, opts);

    CHECK(out.contains("g_global"));
    CHECK(out.contains("heap"));
    CHECK(!out.contains("frame")); // excluded by default

    opts.frame = true;
    CHECK(format_memory_raw(traces, opts).contains("frame"));
}

TEST("memory raw - empty selection says so")
{
    auto t = one_trace({insn_with(0x1000, "mod!foo", {access(0x7ff0, 8, false, true, access_region::frame)})});
    trace const traces[] = {cc::move(t)};

    // Default regions exclude frame, so nothing shows.
    CHECK(format_memory_raw(traces, memory_view_options{}).contains("no accesses"));
}

TEST("memory cachelines - buckets by line, footprint counts distinct bytes")
{
    // Two 8-byte reads into the same 64-byte line: 16 distinct bytes, 2 accesses.
    auto t = one_trace({insn_with(
        0x1000, "mod!foo",
        {access(0x4000, 8, true, false, access_region::heap), access(0x4008, 8, true, false, access_region::heap)})});
    trace const traces[] = {cc::move(t)};

    auto const out = format_memory_cachelines(traces, memory_view_options{});
    CHECK(out.contains("16/64 B"));
    CHECK(out.contains("2 acc"));
}

TEST("memory cachelines - a blank line separates non-adjacent lines")
{
    // 0x4000 and 0x4040 are adjacent lines; 0x8000 is far away.
    auto t = one_trace({insn_with(
        0x1000, "mod!foo",
        {access(0x4000, 8, true, false, access_region::heap), access(0x4040, 8, true, false, access_region::heap),
         access(0x8000, 8, true, false, access_region::heap)})});
    trace const traces[] = {cc::move(t)};

    auto const out = format_memory_cachelines(traces, memory_view_options{});
    // A gap leaves a blank line between the adjacent pair and the far line.
    CHECK(out.contains("\n\n"));
}

TEST("memory cachelines - an access spanning two lines touches both")
{
    // 8 bytes at ...3C crosses the 64-byte boundary: 4 bytes in each of two lines.
    auto t = one_trace({insn_with(0x1000, "mod!foo", {access(0x403C, 8, true, false, access_region::heap)})});
    trace const traces[] = {cc::move(t)};

    auto const out = format_memory_cachelines(traces, memory_view_options{});
    CHECK(out.contains("00004000"));
    CHECK(out.contains("00004040"));
}

TEST("memory stats - groups by the accessing function")
{
    auto t = one_trace({insn_with(0x1000, "mod!foo",
                                  {access(0x4000, 8, true, false, access_region::heap),
                                   access(0x4008, 8, false, true, access_region::heap)}),
                        insn_with(0x2000, "mod!bar", {access(0x5000, 4, true, false, access_region::heap)})});
    trace const traces[] = {cc::move(t)};

    auto const summary = collect_memory_stats(traces, memory_view_options{});
    REQUIRE(summary.rows.size() == 2);

    // foo made the most accesses, so it sorts first.
    CHECK(summary.rows[0].symbol == "mod!foo");
    CHECK(summary.rows[0].accesses == 2);
    CHECK(summary.rows[0].reads == 1);
    CHECK(summary.rows[0].writes == 1);
    CHECK(summary.rows[0].bytes == 16);
    CHECK(summary.rows[0].cachelines == 1); // both in one line

    CHECK(summary.rows[1].symbol == "mod!bar");
    CHECK(summary.rows[1].bytes == 4);

    CHECK(summary.total.accesses == 3);
    CHECK(summary.total.bytes == 20);
    CHECK(summary.total.cachelines == 2);

    auto const out = format_memory_stats(summary);
    CHECK(out.contains("mod!foo"));
    CHECK(out.contains("total"));
}

TEST("memory stats - respects the region filter")
{
    auto t = one_trace({insn_with(
        0x1000, "mod!foo",
        {access(0x4000, 8, true, false, access_region::heap), access(0x7ff0, 8, false, true, access_region::frame)})});
    trace const traces[] = {cc::move(t)};

    // Default excludes frame: only the heap read counts.
    auto const summary = collect_memory_stats(traces, memory_view_options{});
    REQUIRE(summary.rows.size() == 1);
    CHECK(summary.rows[0].accesses == 1);
    CHECK(summary.total.bytes == 8);
}
