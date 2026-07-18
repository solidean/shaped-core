#include <instruction-tracer/report/html_export.hh>
#include <instruction-tracer/report/source_cache.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
recorded_instruction insn_at(u64 rip, cc::string text)
{
    recorded_instruction insn;
    insn.rip = rip;
    insn.length = 4;
    insn.text = cc::move(text);
    return insn;
}

trace one_trace(cc::vector<recorded_instruction> instructions)
{
    trace t;
    t.index = 1;
    t.thread_id = 42;
    t.entry_symbol = "mod.exe!foo";
    t.instructions = cc::move(instructions);
    return t;
}
} // namespace

TEST("html export - self-contained page with embedded data")
{
    auto t = one_trace({insn_at(0x7ff63f4c1008, "mov ecx, [rsp+0x04]")});
    trace const traces[] = {cc::move(t)};

    html_export_meta meta;
    meta.target = "mod.exe!foo";
    source_cache sources;
    auto const out = export_html(traces, meta, sources);

    CHECK(out.starts_with("<!doctype html"));
    CHECK(out.contains("const TRACE_DATA"));
    CHECK(out.contains("<style>"));
    CHECK(out.contains("<div id=\"app\">"));
    CHECK(out.contains("mod.exe!foo"));
}

TEST("html export - addresses are quoted strings, not numbers")
{
    auto t = one_trace({insn_at(0x7ff63f4c1008, "nop")});
    trace const traces[] = {cc::move(t)};

    source_cache sources;
    auto const out = export_html(traces, html_export_meta{}, sources);

    // format_address groups as hi`lo; it must appear inside quotes (a JS number would lose precision).
    CHECK(out.contains("\"00007ff6`3f4c1008\""));
}

TEST("html export - a symbol containing </script> cannot break out")
{
    auto t = one_trace({insn_at(0x1000, "nop")});
    trace tt = cc::move(t);
    tt.entry_symbol = "evil</script><b>";
    trace const traces[] = {cc::move(tt)};

    source_cache sources;
    auto const out = export_html(traces, html_export_meta{}, sources);

    // The raw injection must not appear; the '<' is escaped to <, which the JS parser reverses.
    CHECK(!out.contains("evil</script>"));
    CHECK(out.contains("evil\\u003c/script>"));
}
