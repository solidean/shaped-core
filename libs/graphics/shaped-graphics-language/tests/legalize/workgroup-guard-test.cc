#include "flat-test-support.hh"

#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using namespace sgl::check;

// LEGAL-69: a profile shadows workgroup memory with a bit per scalar, set where one is stored and tested where one is
// read, and `define` zeroes it instead; LEGAL-71 writes the shadow's atomics and the prologue, and EMIT-229 declares it.

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
                                    "@compute(64) fun cs(@thread_id id: int3){work}:\n"
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
            // the bits a range takes are the tree's own arithmetic, and no target writes a helper for them
            CHECK(!text.contains("sgl_written_bits")).dump("text", text);
            // WGSL's workgroup memory starts at zero, so only it has no stride of the four invocations
            auto const clears = text.contains("(k) * 4u");
            CHECK(clears == !is_wgsl).dump("text", text);
        }
        auto const defined = text_of(k_reads_a_hole, target, undefined_guard::define);
        CHECK(!defined.contains("sgl_written")).dump("text", defined);
        auto const zeroes = defined.contains("(k) * 4u");
        CHECK(zeroes == !is_wgsl).dump("text", defined);
    }
}

TEST("sgl guards - the last step writes the shadow's reads and marks as atomics on its words, behind the prologue "
     "(LEGAL-71)")
{
    auto const checked = check_sources(read_prelude(), k_reads_a_hole);
    REQUIRE(reports_of(checked) == "");
    auto const& m = checked.module;
    for (auto const starts_zeroed : {false, true})
    {
        auto const core = legalize(m, m.entry_points[0],
                                   {.expands_workgroup = true,
                                    .workgroup_starts_zeroed = starts_zeroed,
                                    .guards_undefined = undefined_guard::abort});
        CHECK(!find_core_violation(m, core).has_value());
        auto const dump = dump_entry_point(m, core);
        // no call of the shadow is left for a target to write
        CHECK(!dump.contains("workgroup_written")).dump("core", dump);
        CHECK(!dump.contains("workgroup_mark_written")).dump("core", dump);
        CHECK(dump.contains("(written ")).dump("core", dump);
        CHECK(dump.contains("bit_or")).dump("core", dump);
        // the prologue's barrier comes ahead of the body's own; where memory starts at zero there is no prologue
        auto barriers = 0;
        auto const text = cc::string_view(dump);
        for (auto at = text.find("workgroup_barrier"); at >= 0; at = text.find("workgroup_barrier", at + 1))
            ++barriers;
        CHECK(barriers == (starts_zeroed ? 1 : 2)).dump("core", dump);
    }
}

TEST("sgl guards - define zeroes a struct holding an atomic and a matrix, piece by piece, on every target (LEGAL-71)")
{
    constexpr cc::string_view k_mixed = "struct cell:\n"
                                        "    m: mat3\n"
                                        "    v: float2\n"
                                        "\n"
                                        "@workgroup binding tile:\n"
                                        "    cells: cell[8]\n"
                                        "    last: cell\n"
                                        "    counts: atomic[uint][8]\n"
                                        "    count: atomic[uint]\n"
                                        "\n"
                                        "binding work:\n"
                                        "    result: mut buffer[float]\n"
                                        "\n"
                                        "@compute(4) fun cs(@local_thread_index li: int){tile, work}:\n"
                                        "    workgroup_barrier()\n"
                                        "    let n = tile.counts[li].add(1u) + tile.count.load()\n"
                                        "    let column = tile.cells[li].m * float3(1.0)\n"
                                        "    work.result[li] = column.y + tile.last.v.x + float(n)\n";
    for (auto const target : sgl::emit::all_targets())
    {
        auto const text = text_of(k_mixed, target, undefined_guard::define);
        // every target but WGSL zeroes it in the stride, and an atomic only by its store
        CHECK(text.contains("(k) * 4u") == (target != sgl::emit::target::wgsl)).dump("text", text);
    }
}

namespace
{
/// A task and a mesh stage, each of whose invocation 2 stores nothing, which invocation 1 then reads.
constexpr cc::string_view k_task_reads_a_hole = "require mesh_shader\n"
                                                "struct handed:\n"
                                                "    v: float\n"
                                                "@workgroup binding tile:\n"
                                                "    values: float[4]\n"
                                                "@task(4) fun ts(@local_thread_index k: int){tile}:\n"
                                                "    if k != 2:\n"
                                                "        tile.values[k] = float(k)\n"
                                                "    workgroup_barrier()\n"
                                                "    let h: handed = {v = tile.values[(k + 1) % 4]}\n"
                                                "    dispatch_mesh(1u, 1u, 1u, h)\n";
constexpr cc::string_view k_mesh_reads_a_hole
    = "require mesh_shader\n"
      "struct corner:\n"
      "    @position position: hpos4\n"
      "@workgroup binding tile:\n"
      "    values: float[4]\n"
      "@mesh(4, vertices = 4, triangles = 2)\n"
      "fun ms(@local_thread_index k: int, output: mut mesh_output[corner]){tile}:\n"
      "    if k != 2:\n"
      "        tile.values[k] = float(k)\n"
      "    workgroup_barrier()\n"
      "    let v = tile.values[(k + 1) % 4]\n"
      "    output.set_counts(4u, 2u)\n"
      "    output.vertex(k as uint, {position = hpos4(v, 0.0, 0.0, 1.0)})\n"
      "    if k == 0:\n"
      "        output.triangle(0u, uint3(0u, 1u, 2u))\n"
      "        output.triangle(1u, uint3(0u, 2u, 3u))\n";
} // namespace

TEST("sgl guards - a task and a mesh stage shadow their workgroup memory as a compute stage does (LEGAL-69)")
{
    struct stage_case
    {
        cc::string_view source;
        cc::string_view entry;
    };
    for (auto const& c : {stage_case{k_task_reads_a_hole, "ts"}, stage_case{k_mesh_reads_a_hole, "ms"}})
    {
        auto const r = legalize_one(c.source, undefined_guard::report);
        CHECK(r.core.written_words == 1).dump("entry", c.entry);
        REQUIRE(is_valid(r.core.workgroup_index));
        auto found = false;
        for (auto const& message : r.core.messages)
            found = found
                 || (message.pieces.size() == 2 && message.pieces[0] == "workgroup memory `tile.values["
                     && message.pieces[1] == "]` is read before any invocation stored it");
        CHECK(found).dump("core", r.dump);

        // every target with mesh stages writes it, and SGL's own DXIL and SPIR-V too
        for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan, sgl::emit::target::msl})
            for (auto const native : {sgl::emit::native_route::text, sgl::emit::native_route::require})
            {
                if (t == sgl::emit::target::msl && native == sgl::emit::native_route::require)
                    continue;
                auto const text = sgl::compile_to_text({.source = c.source,
                                                        .entry_point = c.entry,
                                                        .target = t,
                                                        .diagnostics = true,
                                                        .guards_undefined = undefined_guard::report,
                                                        .native = native});
                CHECK(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
            }
    }
}

TEST("sgl guards - a site as deep as a program may nest is guarded, its guard past the program's depth (CHK-268, "
     "LEGAL-65)")
{
    // a left-deep sum of `terms` reads whose deepest term is a division, so the division stands as deep as the sum
    auto const source_of = [](int terms)
    {
        auto sum = cc::string("id.x / id.y");
        for (auto i = 1; i < terms; ++i)
            sum += " + id.x";
        return cc::format("binding work:\n"
                          "    result: mut buffer[int]\n"
                          "\n"
                          "@compute(1) fun cs(@thread_id id: int3){{work}}:\n"
                          "    work.result[0] = {}\n",
                          sum);
    };
    // the deepest sum the check pass takes
    auto deepest = 0;
    for (auto terms = 30; terms <= 45; ++terms)
        if (reports_of(check_sources(read_prelude(), source_of(terms))) == "")
            deepest = terms;
    // past what a guard reached before its levels were kept free, 36 of the walks' 40
    CHECK(deepest >= 37);
    auto const r = legalize_one(source_of(deepest), undefined_guard::report);
    CHECK(!r.core.messages.empty()).dump("core", r.dump);
}

TEST("sgl guards - workgroup memory whose shadow would not fit is left unshadowed, and the compile warns of it "
     "(LEGAL-69)")
{
    // 16000 bytes, beside which the shadow's 500 pass 16384
    constexpr auto source = cc::string_view("@workgroup binding tile:\n"
                                            "    values: float[4000]\n"
                                            "\n"
                                            "binding work:\n"
                                            "    result: mut buffer[float]\n"
                                            "\n"
                                            "@compute(4) fun cs(@local_thread_index li: int){tile, work}:\n"
                                            "    tile.values[li] = float(li)\n"
                                            "    workgroup_barrier()\n"
                                            "    work.result[li] = tile.values[(li + 1) % 4]\n");
    auto const warnings_of = [&](undefined_guard guard)
    {
        auto const text = sgl::compile_to_text({.source = source,
                                                .source_name = "big.sgl",
                                                .entry_point = "cs",
                                                .target = sgl::emit::target::hlsl_dx12,
                                                .diagnostics = guard == undefined_guard::report,
                                                .guards_undefined = guard});
        REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
        auto all = cc::string();
        for (auto const& w : text.value().warnings)
            all += w + "\n";
        return all;
    };
    auto const reported = warnings_of(undefined_guard::report);
    CHECK(reported.contains("big.sgl: warning: workgroup-unshadowed: entry point 'cs' holds 16000 bytes of "
                            "workgroup memory"))
        .dump("warnings", reported);
    CHECK(!warnings_of(undefined_guard::none).contains("workgroup-unshadowed"));
    // `define` shadows nothing and zeroes the memory instead, so nothing is left untested
    CHECK(!warnings_of(undefined_guard::define).contains("workgroup-unshadowed"));
}
