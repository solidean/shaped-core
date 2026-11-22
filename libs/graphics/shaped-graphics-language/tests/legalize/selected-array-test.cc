#include "flat-test-support.hh"

#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using namespace sgl::check;

// LEGAL-72: a `.select` array is a local per element, and behaves as the array it stands for under every profile.

namespace
{
constexpr cc::string_view k_edges = "struct frag:\n"
                                    "    a: float\n"
                                    "    b: float\n"
                                    "\n"
                                    "@pixel struct target:\n"
                                    "    color: float4\n"
                                    "\n";

/// The one entry point of `body` behind `k_edges`, run as it is and legalized with `guard`, on `test_inputs`:
/// `p.a` is 0.25 and `p.b` 0.75.
struct runs
{
    outcome structured;
    outcome legalized;
    flat_entry_point core;
    cc::string dump;
};

runs run_both(cc::string_view body, undefined_guard guard = undefined_guard::none)
{
    auto const checked = check_sources(read_prelude(), cc::string(k_edges) + body);
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& m = checked.module;
    auto const& e = m.entry_points[0];
    auto result = runs{.structured = interpret(m, e, test_inputs(m))};
    result.core = legalize(m, e, {.guards_undefined = guard});
    CHECK(!find_core_violation(m, result.core).has_value());
    result.dump = dump_entry_point(m, result.core);
    result.legalized = interpret(m, result.core, test_inputs(m));
    return result;
}

/// `ps` over an array of `how`, read and written at `int(p.b * scale)`, which is 1 at 2.0 and 3 at 4.0.
cc::string indexed(cc::string_view how, cc::string_view scale)
{
    return cc::format("struct span:\n"
                      "    lo: float\n"
                      "    hi: float2\n"
                      "\n"
                      "@pixel fun ps(p: frag) -> target:\n"
                      "    let i = int(p.b * {})\n"
                      "    @indexed(.{}) let mut ends = [span(lo = 1.0, hi = float2(2.0)), span(lo = 3.0, hi = "
                      "float2(4.0)),\n"
                      "                                  span(lo = 5.0, hi = float2(6.0))]\n"
                      "    @indexed(.{}) let mut weights = [0.5, 1.5, 2.5]\n"
                      "    ends[i].hi.y = p.a\n"
                      "    ends[i].lo += 10.0\n"
                      "    weights[i] *= 2.0\n"
                      "    let kept = weights\n"
                      "    weights = [kept[2], kept[1], kept[0]]\n"
                      "    return {{color = float4(ends[i].lo, ends[i].hi.y, weights[i], ends[0].lo + weights[2])}}\n",
                      scale, how, how);
}
} // namespace

TEST("sgl legalize - a .select array behaves as a .memory one, its struct members written in place (LEGAL-72)")
{
    auto const selected = run_both(indexed("select", "2.0"));
    auto const kept = run_both(indexed("memory", "2.0"));
    REQUIRE(selected.structured.status == run_status::ok);
    CHECK(selected.legalized == selected.structured).dump("core", selected.dump);
    CHECK(kept.legalized == kept.structured).dump("core", kept.dump);
    CHECK(selected.structured == kept.structured);
    // the select form holds a local per element, and the memory form the arrays
    CHECK(selected.dump.contains("ends_1")).dump("core", selected.dump);
    CHECK(!kept.dump.contains("ends_1")).dump("core", kept.dump);
}

TEST("sgl legalize - a .select index outside its array does what the profile says, as an array's does (LEGAL-72)")
{
    // abort and report end the invocation before any element is chosen, the report naming the bounds
    for (auto const guard : {undefined_guard::abort, undefined_guard::report})
    {
        auto const r = run_both(indexed("select", "4.0"), guard);
        CHECK(r.structured.status == run_status::program_error);
        CHECK(r.legalized.status == run_status::discarded).dump("core", r.dump);
        if (guard == undefined_guard::report)
        {
            REQUIRE(!r.core.messages.empty());
            CHECK(r.core.messages[0].pieces[1] == " is outside 0 ..< 3");
        }
    }
    // define reads and writes element 0, as it does for an array
    auto const defined = run_both(indexed("select", "4.0"), undefined_guard::define);
    auto const defined_memory = run_both(indexed("memory", "4.0"), undefined_guard::define);
    CHECK(defined.legalized.status == run_status::ok);
    CHECK(defined.legalized == defined_memory.legalized).dump("core", defined.dump);
    // with no profile the index has no value (EVAL-90), and the last element takes it
    auto const unguarded = run_both(indexed("select", "4.0"));
    CHECK(unguarded.structured.status == run_status::program_error);
    CHECK(unguarded.legalized.status == run_status::ok).dump("core", unguarded.dump);
}
