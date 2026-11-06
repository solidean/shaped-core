#include "flat-test-support.hh"

#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using namespace sgl::check;

// LEGAL-69: a profile shadows workgroup memory with a bit per scalar, set where one is stored and tested where one is
// read, and `define` zeroes it instead; EMIT-229 is the text's side.

namespace
{
/// Thread 2 stores nothing, and thread 1 then reads what thread 2 would have stored.
constexpr cc::string_view k_reads_a_hole = "@workgroup binding tile:\n"
                                           "    values: float[4]\n"
                                           "    total: float\n"
                                           "\n"
                                           "binding work:\n"
                                           "    result: mut buffer[float]\n"
                                           "\n"
                                           "@compute(4) fun cs(@local_thread_index li: int){tile, work}:\n"
                                           "    if li != 2:\n"
                                           "        tile.values[li] = float(li)\n"
                                           "    workgroup_barrier()\n"
                                           "    work.result[li] = tile.values[(li + 1) % 4]\n";

struct legalized
{
    checked_sources checked;
    flat_entry_point core;
    cc::string dump;
};

legalized legalize_one(cc::string_view source, undefined_guard guard)
{
    auto result = legalized{.checked = check_sources(read_prelude(), source)};
    REQUIRE(reports_of(result.checked) == "");
    REQUIRE(result.checked.module.entry_points.size() == 1);
    auto const& m = result.checked.module;
    result.core = legalize(m, m.entry_points[0], {.guards_undefined = guard});
    CHECK(!find_core_violation(m, result.core).has_value());
    result.dump = dump_entry_point(m, result.core);
    return result;
}

cc::string text_of(cc::string_view source, sgl::emit::target target, undefined_guard guard)
{
    auto const text = sgl::compile_to_text({.source = source,
                                            .entry_point = "cs",
                                            .target = target,
                                            .diagnostics = guard == undefined_guard::report,
                                            .guards_undefined = guard});
    REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
    return text.value().text;
}
} // namespace

TEST("sgl guards - a read of workgroup memory tests the shadow, and its report names the place (LEGAL-69)")
{
    auto const r = legalize_one(k_reads_a_hole, undefined_guard::report);
    // five scalars, one word
    CHECK(r.core.written_words == 1);
    CHECK(!r.core.zeroes_workgroup);
    REQUIRE(is_valid(r.core.workgroup_index));
    CHECK(r.dump.contains("workgroup_written_word")).dump("core", r.dump);
    CHECK(r.dump.contains("(call:effect workgroup_mark_written")).dump("core", r.dump);
    auto found = false;
    for (auto const& message : r.core.messages)
        if (message.pieces.size() == 2 && message.pieces[0] == "workgroup memory `tile.values["
            && message.pieces[1] == "]` is read before any invocation stored it")
            found = true;
    CHECK(found).dump("core", r.dump);
}

TEST("sgl guards - the stage's own local_thread_index strides the prologue, and one is added where it has none")
{
    auto const own = legalize_one(k_reads_a_hole, undefined_guard::abort);
    REQUIRE(own.core.stage_inputs.size() == 1);
    CHECK(own.core.workgroup_index == own.core.stage_inputs[0].local);

    auto const added = legalize_one("@workgroup binding tile:\n"
                                    "    total: uint\n"
                                    "\n"
                                    "@compute(8) fun cs(@thread_id id: int3){tile}:\n"
                                    "    if id.x == 0:\n"
                                    "        tile.total = 3u\n"
                                    "    workgroup_barrier()\n"
                                    "    print \"$(tile.total)\"\n",
                                    undefined_guard::abort);
    REQUIRE(added.core.stage_inputs.size() == 2);
    CHECK(added.core.stage_inputs[1].input == stage_input::local_thread_index);
    CHECK(added.core.workgroup_index == added.core.stage_inputs[1].local);
}

TEST("sgl guards - no profile, and a stage without workgroup memory, shadow nothing")
{
    auto const none = legalize_one(k_reads_a_hole, undefined_guard::none);
    CHECK(none.core.written_words == 0);
    CHECK(!is_valid(none.core.workgroup_index));
    CHECK(!none.dump.contains("workgroup_written"));

    auto const plain = legalize_one("binding work:\n"
                                    "    result: mut buffer[int]\n"
                                    "\n"
                                    "@compute(4) fun cs(@thread_id id: int3){work}:\n"
                                    "    work.result[id.x] = 7 / id.x\n",
                                    undefined_guard::report);
    CHECK(plain.core.written_words == 0);
    CHECK(!is_valid(plain.core.workgroup_index));
}

TEST("sgl guards - a field, a swizzle and an atomic reach their own scalars, and a store of an atomic tests nothing")
{
    auto const r = legalize_one("@workgroup binding tile:\n"
                                "    pad: float2\n"
                                "    v: float3\n"
                                "    count: atomic[uint]\n"
                                "\n"
                                "binding work:\n"
                                "    result: mut buffer[float]\n"
                                "\n"
                                "@compute(4) fun cs(@local_thread_index li: int){tile, work}:\n"
                                "    if li == 0:\n"
                                "        tile.count.store(0u)\n"
                                "        tile.v.y = 1.0\n"
                                "        tile.v.z = 2.0\n"
                                "    workgroup_barrier()\n"
                                "    let n = tile.count.add(1u)\n"
                                "    let zy = tile.v.zy\n"
                                "    work.result[li] = zy.x + zy.y + float(n)\n",
                                undefined_guard::report);
    // pad is scalars 0 and 1, v 2 to 4, count 5: `v.y` is bit 3, `v.z` bit 4, and count bit 5
    CHECK(r.dump.contains("(lit 8u : uint)")).dump("core", r.dump);
    CHECK(r.dump.contains("(lit 16u : uint)")).dump("core", r.dump);
    CHECK(r.dump.contains("(lit 32u : uint)")).dump("core", r.dump);
    auto places = cc::vector<cc::string>();
    for (auto const& message : r.core.messages)
        if (message.pieces.size() == 1)
            places.push_back(message.pieces[0]);
    // the add reads the count, the store does not; the swizzle reads each of its fields
    CHECK(places.size() == 3).dump("core", r.dump);
    CHECK(places.values().contains(cc::string("workgroup memory `tile.count` is read before any invocation stored "
                                              "it")));
    CHECK(places.values().contains(cc::string("workgroup memory `tile.v.zy` is read before any invocation stored it")));
}

TEST("sgl guards - a range past four words of the shadow is tested and marked in a loop")
{
    auto const r = legalize_one("@workgroup binding tile:\n"
                                "    a: float[100]\n"
                                "    b: float[100]\n"
                                "\n"
                                "@compute(100) fun cs(@local_thread_index li: int){tile}:\n"
                                "    tile.a[li] = float(li)\n"
                                "    workgroup_barrier()\n"
                                "    if li == 0:\n"
                                "        tile.b = tile.a\n",
                                undefined_guard::abort);
    CHECK(r.core.written_words == 7);
    CHECK(r.dump.contains("(for")).dump("core", r.dump);
}

TEST("sgl guards - a shadow that would not fit beside the memory it shadows is left out")
{
    auto const r = legalize_one("@workgroup binding tile:\n"
                                "    a: float[4096]\n"
                                "\n"
                                "@compute(64) fun cs(@local_thread_index li: int){tile}:\n"
                                "    tile.a[li] = float(li)\n"
                                "    workgroup_barrier()\n"
                                "    print \"$(tile.a[0])\"\n",
                                undefined_guard::report);
    CHECK(r.core.written_words == 0);
    CHECK(!r.dump.contains("workgroup_written"));
}

TEST("sgl guards - define zeroes workgroup memory instead, and tests no read")
{
    auto const r = legalize_one(k_reads_a_hole, undefined_guard::define);
    CHECK(r.core.zeroes_workgroup);
    CHECK(r.core.written_words == 0);
    CHECK(is_valid(r.core.workgroup_index));
    CHECK(!r.dump.contains("workgroup_written"));
}

TEST("sgl guards - a program that stores before it reads behaves as it does unguarded")
{
    auto const checked = check_sources(read_prelude(), "@workgroup binding tile:\n"
                                                       "    values: float[4]\n"
                                                       "    count: atomic[uint]\n"
                                                       "\n"
                                                       "@compute(1) fun cs(@local_thread_index li: int){tile}:\n"
                                                       "    tile.count.store(2u)\n"
                                                       "    for i in 0 ..< 4:\n"
                                                       "        tile.values[i] = float(i)\n"
                                                       "    workgroup_barrier()\n"
                                                       "    let n = tile.count.add(1u)\n"
                                                       "    print \"$(tile.values[3]) $n\"\n");
    REQUIRE(reports_of(checked) == "");
    auto const& m = checked.module;
    auto const& e = m.entry_points[0];
    auto const structured = interpret(m, e, {});
    REQUIRE(structured.status == run_status::ok);
    for (auto const guard : {undefined_guard::abort, undefined_guard::report})
    {
        auto const core = legalize(m, e, {.guards_undefined = guard});
        CHECK(!find_core_violation(m, core).has_value());
        auto const guarded = interpret(m, core, {});
        // the shadow's own calls have an effect, which the trace records, so the rest is what must agree
        CHECK(guarded.status == structured.status).dump("core", dump_entry_point(m, core));
        CHECK(guarded.result == structured.result);
        CHECK(sgl::ast::impl::is_equal(guarded.printed, structured.printed));
        CHECK(sgl::ast::impl::is_equal(guarded.buffers, structured.buffers));
    }
}

TEST("sgl guards - every target writes the shadow, and every target but WGSL clears it before the body (EMIT-229)")
{
    for (auto const target : sgl::emit::all_targets())
    {
        auto const is_wgsl = target == sgl::emit::target::wgsl;
        for (auto const guard : {undefined_guard::abort, undefined_guard::report})
        {
            auto const text = text_of(k_reads_a_hole, target, guard);
            auto const declares = text.contains("sgl_written[1]") || text.contains("sgl_written: array<atomic<u32>, 1>");
            CHECK(declares).dump("text", text);
            CHECK(text.contains("sgl_written_bits")).dump("text", text);
            // WGSL's workgroup memory starts at zero
            auto const clears = text.contains("+= 4u)");
            CHECK(clears == !is_wgsl).dump("text", text);
        }
        auto const defined = text_of(k_reads_a_hole, target, undefined_guard::define);
        CHECK(!defined.contains("sgl_written")).dump("text", defined);
        auto const zeroes = defined.contains("+= 4u)");
        CHECK(zeroes == !is_wgsl).dump("text", defined);
    }
}
