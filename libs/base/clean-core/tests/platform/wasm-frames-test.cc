#include <clean-core/platform/impl/wasm_frames.hh>
#include <nexus/test.hh>

// The frame-text parser, tested against recorded engine output rather than against a live runtime.
//
// The strings below are the contract, and they are a contract with V8 and SpiderMonkey rather than with Emscripten.
// So they are checked in as samples: a wasm run could only ever assert about the engine it happens to be under,
// and the format that breaks us is the one on the engine nobody ran.
//
// Every V8 sample here was produced under node 22 and emscripten 6.0.1, and pasted verbatim.
// The SpiderMonkey samples follow its documented `NAME@URL:line:col` spelling.

using cc::impl::parse_wasm_frame;

TEST("wasm_frames - V8 names a wasm frame by module, index and code offset")
{
    auto const f
        = parse_wasm_frame("    at probe_o0.wasm.deep2() (wasm://wasm/probe_o0.wasm-0001957a:wasm-function[11]:0x4d1)");

    REQUIRE(f.has_value());
    CHECK(!f.value().is_js);
    CHECK(f.value().function_index == 11);
    CHECK(f.value().module_offset == 0x4d1);
    CHECK(f.value().name == "deep2()");
    CHECK(f.value().module == "wasm://wasm/probe_o0.wasm-0001957a");
    CHECK(f.value().address() == 0x4d1);
}

TEST("wasm_frames - a stripped build keeps its offset and loses only the name")
{
    // Byte-for-byte the same module and the same -O0 build as the sample above, linked without --profiling-funcs.
    // The offsets matching is the whole basis for resolving a stripped build after the fact.
    auto const named
        = parse_wasm_frame("    at probe_o0.wasm.deep2() (wasm://wasm/probe_o0.wasm-0001957a:wasm-function[11]:0x4d1)");
    auto const stripped = parse_wasm_frame("    at wasm://wasm/000182ce:wasm-function[11]:0x4d1");

    REQUIRE(named.has_value());
    REQUIRE(stripped.has_value());

    CHECK(stripped.value().module_offset == named.value().module_offset);
    CHECK(stripped.value().function_index == named.value().function_index);
    CHECK(stripped.value().name.empty());
    CHECK(!stripped.value().is_js);
}

TEST("wasm_frames - SpiderMonkey's spelling parses to the same frame")
{
    auto const f = parse_wasm_frame("deep2()@wasm://wasm/probe_o0.wasm-0001957a:wasm-function[11]:0x4d1");

    REQUIRE(f.has_value());
    CHECK(!f.value().is_js);
    CHECK(f.value().function_index == 11);
    CHECK(f.value().module_offset == 0x4d1);
    CHECK(f.value().name == "deep2()");
}

TEST("wasm_frames - a JS frame is reported as one, with its line as the address")
{
    auto const f = parse_wasm_frame("    at callMain (C:\\work\\probe_o0.js:1917:15)");

    REQUIRE(f.has_value());
    CHECK(f.value().is_js);
    CHECK(f.value().module_offset == 1917);
    CHECK(f.value().name == "callMain");
    CHECK(f.value().module == "C:\\work\\probe_o0.js");

    // Tagged, so a symbolizer never resolves a JS line against the module's code section.
    CHECK(f.value().address() == (1917u | cc::impl::wasm_js_frame_bit));
}

TEST("wasm_frames - an anonymous JS frame has a location and no name")
{
    auto const f = parse_wasm_frame("    at C:\\work\\probe_o0.js:623:12");

    REQUIRE(f.has_value());
    CHECK(f.value().is_js);
    CHECK(f.value().module_offset == 623);
    CHECK(f.value().name.empty());
}

TEST("wasm_frames - the module prefix comes off a name and a genuine dotted name survives")
{
    auto const stripped
        = parse_wasm_frame("    at app.wasm.render_frame (wasm://wasm/app.wasm-0012cc2a:wasm-function[26]:0x9f3)");
    REQUIRE(stripped.has_value());
    CHECK(stripped.value().name == "render_frame");

    // The prefix is only removed when it names THIS module, so a function whose own name starts with a dotted
    // segment keeps it.
    auto const kept
        = parse_wasm_frame("    at other.render_frame (wasm://wasm/app.wasm-0012cc2a:wasm-function[26]:0x9f3)");
    REQUIRE(kept.has_value());
    CHECK(kept.value().name == "other.render_frame");
}

TEST("wasm_frames - lines that name no frame parse to nothing")
{
    CHECK(!parse_wasm_frame("").has_value());
    CHECK(!parse_wasm_frame("   ").has_value());
    CHECK(!parse_wasm_frame("Error").has_value());
    CHECK(!parse_wasm_frame("    ... collapsed 59 duplicate lines matching above 1 lines 59 times...").has_value());
    CHECK(!parse_wasm_frame("    at somewhere (no-colons-here)").has_value());
}

TEST("wasm_frames - the legacy V8 spelling is rejected rather than misread")
{
    // Node 10 and friends wrote `wasm-function[N]:M`, where M counts bytes INSIDE the function rather than into the
    // module.
    // Reading it as a code offset would resolve to a confidently wrong place, so it parses to nothing.
    CHECK(!parse_wasm_frame("    at app.wasm.render (wasm://wasm/app.wasm-0012cc2a:wasm-function[26]:41)").has_value());
}

TEST("wasm_frames - an offset too large for the address space is rejected")
{
    CHECK(!parse_wasm_frame("    at app.wasm.f (wasm://wasm/app-1:wasm-function[1]:0x1FFFFFFFF)").has_value());
    CHECK(!parse_wasm_frame("    at app.wasm.f (wasm://wasm/app-1:wasm-function[99999999999]:0x10)").has_value());
}
