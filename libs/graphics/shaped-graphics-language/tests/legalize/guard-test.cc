#include "flat-test-support.hh"

#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using namespace sgl::check;

// LEGAL-65: an operation that may have no value tests its operands first, on the interpreter as on a GPU.

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
    outcome guarded;
    flat_entry_point core;
    cc::string dump;
};

runs run_both(cc::string_view body, undefined_guard guard)
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
    result.guarded = interpret(m, result.core, test_inputs(m));
    return result;
}

/// The legalized dump of the one entry point of `source`, with `guard`.
cc::string guarded_dump(cc::string_view source, undefined_guard guard = undefined_guard::abort)
{
    auto const checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const core = legalize(checked.module, checked.module.entry_points[0], {.guards_undefined = guard});
    CHECK(!find_core_violation(checked.module, core).has_value());
    return dump_entry_point(checked.module, core);
}

constexpr cc::string_view k_divides_by_zero = "@pixel fun ps(p: frag) -> target:\n"
                                              "    let d = int(p.a)\n"
                                              "    let q = 7 / d\n"
                                              "    return {\n"
                                              "        color = float4(float(q), 0.0, 0.0, 1.0)\n"
                                              "    }\n";

constexpr cc::string_view k_indexes_past_the_end = "@pixel fun ps(p: frag) -> target:\n"
                                                   "    let table = [1.0, 2.0, 3.0]\n"
                                                   "    let i = int(p.b * 8.0)\n"
                                                   "    return {\n"
                                                   "        color = float4(table[i], 0.0, 0.0, 1.0)\n"
                                                   "    }\n";
} // namespace

TEST("sgl guards - abort ends the invocation where a division by zero would run, and reports nothing")
{
    auto const r = run_both(k_divides_by_zero, undefined_guard::abort);
    CHECK(r.structured.status == run_status::program_error);
    CHECK(r.guarded.status == run_status::discarded).dump("core", r.dump);
    CHECK(r.guarded.printed.empty());
    CHECK(r.core.messages.empty());
}

TEST("sgl guards - report prints the operation and its operands as a failed assert, then ends the invocation")
{
    auto const r = run_both(k_divides_by_zero, undefined_guard::report);
    CHECK(r.guarded.status == run_status::discarded).dump("core", r.dump);
    REQUIRE(r.core.messages.size() == 1);
    CHECK(r.core.messages[0].is_assert);
    CHECK(r.core.messages[0].pieces.size() == 3);
    CHECK(r.core.messages[0].pieces[1] == " / ");
    REQUIRE(r.guarded.printed.size() == 1);
    REQUIRE(r.guarded.printed[0].values.size() == 2);
    CHECK(r.guarded.printed[0].values[0].leaves[0].as_int() == 7);
    CHECK(r.guarded.printed[0].values[1].leaves[0].as_int() == 0);
}

TEST("sgl guards - an array index outside the array ends the invocation, and its report names the bounds")
{
    auto const r = run_both(k_indexes_past_the_end, undefined_guard::report);
    CHECK(r.structured.status == run_status::program_error);
    CHECK(r.guarded.status == run_status::discarded).dump("core", r.dump);
    REQUIRE(r.core.messages.size() == 1);
    CHECK(r.core.messages[0].pieces[1] == " is outside 0 ..< 3");
    REQUIRE(r.guarded.printed.size() == 1);
    CHECK(r.guarded.printed[0].values[0].leaves[0].as_int() == 6);
}

TEST("sgl guards - a program that never fails a guard behaves as it does unguarded")
{
    constexpr auto in_range = "@pixel fun ps(p: frag) -> target:\n"
                              "    let table = [1.0, 2.0, 3.0]\n"
                              "    let d = int(p.b * 4.0) - 1\n"
                              "    let q = -9 / d % 4\n"
                              "    return {\n"
                              "        color = float4(table[d], float(q), 0.0, 1.0)\n"
                              "    }\n";
    for (auto const guard : {undefined_guard::abort, undefined_guard::report})
    {
        auto const r = run_both(in_range, guard);
        CHECK(r.structured.status == run_status::ok);
        CHECK(r.guarded == r.structured).dump("core", r.dump);
        CHECK(r.dump.contains("divisor")).dump("core", r.dump);
    }
}

TEST("sgl guards - none leaves the tree as it is")
{
    auto const r = run_both(k_divides_by_zero, undefined_guard::none);
    CHECK(!r.dump.contains("divisor"));
    CHECK(r.guarded.status == run_status::program_error);
}

TEST("sgl guards - a literal divisor that can never fail needs no guard, and -1 under an int does")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = id.x / 4 + id.x % 3\n";
    CHECK(!guarded_dump(source).contains("divisor"));

    constexpr auto negated = "binding work:\n"
                             "    values: mut buffer[int]\n"
                             "\n"
                             "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                             "    work.values[id.x] = id.x / -1\n";
    CHECK(guarded_dump(negated).contains("divisor"));

    constexpr auto unsigned_negated = "binding work:\n"
                                      "    values: mut buffer[uint]\n"
                                      "\n"
                                      "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = (id.x as uint) / 4294967295u\n";
    CHECK(!guarded_dump(unsigned_negated).contains("divisor"));
}

TEST("sgl guards - before a barrier a guard sets a flag and a safe operand, and the invocation ends after the barrier")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let q = work.values[0] / id.x\n"
                            "    workgroup_barrier()\n"
                            "    work.values[id.x] = q / id.y\n";
    auto const dump = guarded_dump(source);
    CHECK(dump.contains("aborted")).dump("core", dump);
    auto const barrier = dump.find("workgroup_barrier");
    auto const first_return = dump.find("(return");
    REQUIRE(barrier >= 0);
    // the first guard waits for the barrier, and the one behind it ends at once
    CHECK(first_return > barrier).dump("core", dump);
    CHECK(dump.find("(return", first_return + 1) > first_return).dump("core", dump);
}

TEST("sgl guards - without a barrier, a derivative or a subgroup operation no flag is declared")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = work.values[0] / id.x\n";
    auto const dump = guarded_dump(source);
    CHECK(dump.contains("divisor"));
    CHECK(!dump.contains("aborted")).dump("core", dump);
}

TEST("sgl guards - every target writes the guards of vectors, spreads and indices")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "    lanes: mut buffer[uint]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let table = [3, 1, 4, 1, 5]\n"
                            "    let v = int3(work.values[0], 2, 3) / id\n"
                            "    let w = v % id.x\n"
                            "    let u = (id.x as uint) / (work.values[1] as uint)\n"
                            "    workgroup_barrier()\n"
                            "    work.values[id.x] = w.x + w.y + table[work.values[2]]\n"
                            "    work.lanes[id.x] = u % 7u\n";
    for (auto const target : sgl::emit::all_targets())
        for (auto const guard : {undefined_guard::abort, undefined_guard::report})
        {
            auto const text = sgl::compile_to_text({.source = source,
                                                    .entry_point = "cs",
                                                    .target = target,
                                                    .diagnostics = guard == undefined_guard::report,
                                                    .guards_undefined = guard});
            REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
            CHECK(text.value().text.contains("divisor")).dump("text", text.value().text);
            CHECK(text.value().text.contains("aborted")).dump("text", text.value().text);
        }
}

TEST("sgl guards - with diagnostics an assert before a barrier sets the flag, and its invocation ends after the "
     "barrier "
     "(LEGAL-64)")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let v = work.values[id.x]\n"
                            "    assert v >= 0, \"negative $v\"\n"
                            "    workgroup_barrier()\n"
                            "    assert v < 100, \"large $v\"\n"
                            "    work.values[id.x] = v * 2\n";
    auto const checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    auto const core = legalize(checked.module, checked.module.entry_points[0], {.lowers_asserts = true});
    CHECK(!find_core_violation(checked.module, core).has_value());
    auto const dump = dump_entry_point(checked.module, core);
    CHECK(dump.contains("aborted")).dump("core", dump);
    auto const barrier = dump.find("workgroup_barrier");
    REQUIRE(barrier >= 0);
    // both asserts still print, the first without ending its invocation before every one reached the barrier
    CHECK(dump.find("(print") < barrier).dump("core", dump);
    CHECK(dump.find("(return") > barrier).dump("core", dump);
    REQUIRE(core.messages.size() == 2);
    CHECK(core.messages[0].is_assert);
    CHECK(core.messages[1].is_assert);
}
