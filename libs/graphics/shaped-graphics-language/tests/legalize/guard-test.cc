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
    // the divisor is pinned once, under the name its buffer index's guard gave it
    CHECK(dump.contains("(call equal_int")).dump("core", dump);
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

TEST("sgl guards - once the flag is set, a loop without a barrier is left, and one holding a barrier runs on")
{
    // a divisor of 0 trips every invocation, whose safe divisor of 1 would make the first loop run `total` times
    constexpr auto source = "binding params:\n"
                            "    total: int\n"
                            "    parts: int\n"
                            "\n"
                            "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@workgroup binding shared:\n"
                            "    total: atomic[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){params, work, shared}:\n"
                            "    let steps = params.total / params.parts\n"
                            "    let mut sum = 0\n"
                            "    for i in 0 ..< steps:\n"
                            "        sum = sum + i\n"
                            "    for round in 0 ..< 4:\n"
                            "        shared.total.add(sum)\n"
                            "        workgroup_barrier()\n"
                            "    work.values[id.x] = sum\n";
    auto const dump = guarded_dump(source);
    auto const first = dump.find("(for");
    REQUIRE(first >= 0);
    auto const second = dump.find("(for", first + 1);
    REQUIRE(second > first);
    auto const between = cc::string_view(dump).subview({.offset = first, .size = second - first});
    auto const after = cc::string_view(dump).subview({.offset = second, .size = dump.size() - second});
    CHECK(between.contains("aborted")).dump("core", dump);
    CHECK(between.contains("(break")).dump("core", dump);
    auto const barrier = after.find("workgroup_barrier");
    REQUIRE(barrier >= 0);
    CHECK(!after.subview({.offset = 0, .size = barrier}).contains("(break")).dump("core", dump);
}

TEST("sgl guards - a maths builtin with no value for its operands ends the invocation, and one with a value runs alike")
{
    // `p.a` is 0.25 and `p.b` 0.75; each `bad` has no value by EVAL-87, each `good` has one
    struct one_case
    {
        cc::string_view bad;
        cc::string_view good;
    };
    one_case const cases[] = {
        {"pow(p.a - 1.0, 2.0)", "pow(p.a, 2.0)"},
        {"pow(p.a - 0.25, -1.0)", "pow(p.b, -1.0)"},
        {"asin(p.b * 2.0)", "asin(p.b)"},
        {"acos(p.a - 2.0)", "acos(p.a)"},
        {"atan2(p.a - 0.25, p.b - 0.75)", "atan2(p.a, p.b)"},
        {"smoothstep(p.b, p.a, 0.5)", "smoothstep(p.a, p.b, 0.5)"},
        {"smoothstep(p.a, p.a, 0.5)", "smoothstep(p.a, p.b, 0.6)"},
        {"clamp(0.5, p.b, p.a)", "clamp(0.5, p.a, p.b)"},
        {"float(clamp(int(p.b * 4.0), 3, 1))", "float(clamp(int(p.b * 4.0), 1, 3))"},
    };
    for (auto const& c : cases)
    {
        auto const program = [&](cc::string_view call)
        {
            return cc::format("@pixel fun ps(p: frag) -> target:\n"
                              "    let v = {}\n"
                              "    return {{\n"
                              "        color = float4(v, 0.0, 0.0, 1.0)\n"
                              "    }}\n",
                              call);
        };
        auto const bad = run_both(program(c.bad), undefined_guard::report);
        CHECK(bad.structured.status == run_status::program_error).dump("call", c.bad);
        CHECK(bad.guarded.status == run_status::discarded).dump("call", c.bad).dump("core", bad.dump);
        CHECK(bad.guarded.printed.size() == 1).dump("call", c.bad);

        auto const good = run_both(program(c.good), undefined_guard::abort);
        CHECK(good.structured.status == run_status::ok).dump("call", c.good);
        CHECK(good.guarded == good.structured).dump("call", c.good).dump("core", good.dump);
    }
}

TEST("sgl guards - every target writes the guards of the maths builtins, scalar, vector and half")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float4]\n"
                            "\n"
                            "require shader_f16\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let v = work.values[id.x]\n"
                            "    let h = half(v.x)\n"
                            "    let a = pow(v, v) + asin(v) + acos(v.zyxw) + atan2(v, v.wzyx)\n"
                            "    let b = smoothstep(v.x, v.y, v.z) + clamp(v.w, v.x, v.y) + float(pow(h, h))\n"
                            "    work.values[id.x] = a + float4(b, 0.0, 0.0, 0.0)\n";
    for (auto const t : sgl::emit::all_targets())
    {
        auto const text
            = sgl::compile_to_text({.source = source,
                                    .entry_point = "cs",
                                    .target = t,
                                    .guards_undefined = undefined_guard::abort,
                                    .device_features = sgl::check::feature_set(sgl::check::feature::shader_f16)});
        REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
        CHECK(text.value().text.contains("operand")).dump("text", text.value().text);
    }
}

namespace
{
constexpr cc::string_view k_store = "binding store:\n"
                                    "    data: mut buffer[float]\n"
                                    "    counts: mut buffer[int]\n"
                                    "\n";
} // namespace

TEST("sgl guards - a buffer index past the bound buffer's end ends the invocation, and its report names the length "
     "(LEGAL-66)")
{
    // p.b is 0.75, so i is 6, and the run's buffers hold 4 elements
    auto const r = run_both(cc::string(k_store)
                                + "@pixel fun ps(p: frag){store} -> target:\n"
                                  "    let i = int(p.b * 8.0)\n"
                                  "    return {\n"
                                  "        color = float4(store.data[i], 0.0, 0.0, 1.0)\n"
                                  "    }\n",
                            undefined_guard::report);
    CHECK(r.structured.status == run_status::program_error);
    CHECK(r.guarded.status == run_status::discarded).dump("core", r.dump);
    REQUIRE(r.core.messages.size() == 1);
    CHECK(r.core.messages[0].pieces[1] == " is outside a buffer of ");
    REQUIRE(r.guarded.printed.size() == 1);
    CHECK(r.guarded.printed[0].values[0].leaves[0].as_int() == 6);
    CHECK(r.guarded.printed[0].values[1].leaves[0].as_uint() == u32(k_store_elements));
}

TEST("sgl guards - a buffer index and a store inside the buffer behave as they do unguarded")
{
    for (auto const guard : {undefined_guard::abort, undefined_guard::report})
    {
        auto const r = run_both(cc::string(k_store)
                                    + "@pixel fun ps(p: frag){store} -> target:\n"
                                      "    let i = uint(p.b * 4.0)\n"
                                      "    store.counts[i] = store.counts[i - 1u] + 1\n"
                                      "    return {\n"
                                      "        color = float4(store.data[i], 0.0, 0.0, 1.0)\n"
                                      "    }\n",
                                guard);
        CHECK(r.structured.status == run_status::ok);
        CHECK(r.guarded == r.structured).dump("core", r.dump);
        CHECK(r.dump.contains("(length")).dump("core", r.dump);
    }
}

TEST("sgl - a buffer's length is how many elements the bound buffer holds (CHK-460)")
{
    auto const checked = check_sources(read_prelude(), cc::string(k_edges) + k_store
                                                           + "@pixel fun ps(p: frag){store} -> target:\n"
                                                             "    let n = store.data.length\n"
                                                             "    return {\n"
                                                             "        color = float4(float(n), 0.0, 0.0, 1.0)\n"
                                                             "    }\n");
    REQUIRE(reports_of(checked) == "");
    auto const& m = checked.module;
    auto const run = interpret(m, m.entry_points[0], test_inputs(m));
    REQUIRE(run.status == run_status::ok);
    CHECK(run.result.leaves[0].as_float() == float(k_store_elements));
}

TEST("sgl emit - a buffer's length is read through GetDimensions in HLSL, arrayLength in WGSL, and its group's lengths "
     "in "
     "MSL (EMIT-195)")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "    raw: bytes\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    work.values[0] = float(work.values.length + work.raw.length)\n";
    auto const text_for = [&](sgl::emit::target t)
    { return sgl::compile_to_text({.source = source, .entry_point = "cs", .target = t}); };
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const hlsl = text_for(t);
        REQUIRE(hlsl.has_value()).dump("error", hlsl.has_error() ? hlsl.error() : cc::string());
        CHECK(hlsl.value().text.contains(".GetDimensions(n, stride);")).dump("text", hlsl.value().text);
        CHECK(hlsl.value().text.contains(".GetDimensions(n);")).dump("text", hlsl.value().text);
    }
    auto const wgsl = text_for(sgl::emit::target::wgsl);
    REQUIRE(wgsl.has_value()).dump("error", wgsl.has_error() ? wgsl.error() : cc::string());
    CHECK(wgsl.value().text.contains("arrayLength(&")).dump("text", wgsl.value().text);
    CHECK(wgsl.value().text.contains(") * 4u)")).dump("text", wgsl.value().text);
    // sg writes each slot's byte length behind the group's slots: an element count divides it, a bytes' is it
    auto const msl = text_for(sgl::emit::target::msl);
    REQUIRE(msl.has_value()).dump("error", msl.has_error() ? msl.error() : cc::string());
    CHECK(msl.value().text.contains("    uint sgl_lengths [[id(2)]][2];\n")).dump("text", msl.value().text);
    CHECK(msl.value().text.contains("(work_group.sgl_lengths[0] / uint(sizeof(work_values[0])))"))
        .dump("text", msl.value().text);
    CHECK(msl.value().text.contains("work_group.sgl_lengths[1]")).dump("text", msl.value().text);
}

TEST("sgl guards - a bytes load past the end or off a word ends the invocation, and one inside runs alike (LEGAL-66)")
{
    // p.b is 0.75, so `at` is the offset plus 3; the memory holds four words, 16 bytes
    auto const run = [](cc::string_view offset)
    {
        auto const checked
            = check_sources(read_prelude(), cc::string(k_edges)
                                                + cc::format("binding work:\n"
                                                             "    raw: mut bytes\n"
                                                             "\n"
                                                             "@pixel fun ps(p: frag){{work}} -> target:\n"
                                                             "    let at = {} + uint(p.b * 4.0)\n"
                                                             "    let v = work.raw.load2(at).y\n"
                                                             "    work.raw.store(at, v + 1u)\n"
                                                             "    return {{\n"
                                                             "        color = float4(float(v), 0.0, 0.0, 1.0)\n"
                                                             "    }}\n",
                                                             offset));
        REQUIRE(reports_of(checked) == "");
        auto const& m = checked.module;
        auto inputs = test_inputs(m);
        for (auto i = isize(0); i < m.symbols.size(); ++i)
            if (m.symbols[i].kind == symbol_kind::binding && m.symbols[i].name == "work")
            {
                auto raw = buffer_contents{.binding = symbol_id(i), .member = 0};
                for (auto w = 0u; w < 4u; ++w)
                    raw.leaves.push_back(scalar::of_uint(10u + w));
                inputs.buffers.push_back(cc::move(raw));
            }
        auto const& e = m.entry_points[0];
        auto const core = legalize(m, e, {.guards_undefined = undefined_guard::abort});
        CHECK(!find_core_violation(m, core).has_value());
        return cc::pair(interpret(m, e, inputs), interpret(m, core, inputs));
    };
    // 1 + 3 = 4: words 1 and 2, inside
    auto const [inside, inside_guarded] = run("1u");
    CHECK(inside.status == run_status::ok);
    CHECK(inside_guarded == inside);
    // 6 + 3 = 9: off a word
    auto const [unaligned, unaligned_guarded] = run("6u");
    CHECK(unaligned.status == run_status::program_error);
    CHECK(unaligned_guarded.status == run_status::discarded);
    // 9 + 3 = 12: word 3 is the last, and word 4 is past it
    auto const [past, past_guarded] = run("9u");
    CHECK(past.status == run_status::program_error);
    CHECK(past_guarded.status == run_status::discarded);
}

TEST("sgl guards - once a guard may set the flag, every write before the barrier is skipped where it is set (LEGAL-67)")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "    hits: mut buffer[atomic[uint]]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let q = 7 / id.x\n"
                            "    work.values[id.x] = q\n"
                            "    work.hits[0].add(1u)\n"
                            "    workgroup_barrier()\n"
                            "    work.values[id.y] = q\n";
    auto const dump = guarded_dump(source);
    auto masks = 0;
    for (auto at = dump.find("(not (local aborted"); at >= 0; at = dump.find("(not (local aborted", at + 1))
        ++masks;
    // the store and the atomic before the barrier; the barrier itself is never skipped, and past the end-test of the
    // flag the invocation has ended
    CHECK(masks == 2).dump("core", dump);
    CHECK(dump.contains("workgroup_barrier")).dump("core", dump);
}

TEST("sgl guards - without a guard that may set the flag no write is masked")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    work.values[0] = 7\n"
                            "    workgroup_barrier()\n";
    auto const checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    auto const core = legalize(checked.module, checked.module.entry_points[0],
                               {.guards_undefined = undefined_guard::abort, .guards_buffers = false});
    CHECK(!dump_entry_point(checked.module, core).contains("aborted"));
}

TEST("sgl guards - define gives a division by zero WGSL's value, and an index past the end the first element "
     "(LEGAL-68)")
{
    // p.a is 0.25, so d is 0; p.b is 0.75, so i is 6, past the table's 3
    auto const r = run_both("@pixel fun ps(p: frag) -> target:\n"
                            "    let d = int(p.a)\n"
                            "    let table = [1.0, 2.0, 3.0]\n"
                            "    let i = int(p.b * 8.0)\n"
                            "    return {\n"
                            "        color = float4(float(7 / d), float(7 % d), table[i], pow(p.a - 1.0, 0.5))\n"
                            "    }\n",
                            undefined_guard::define);
    CHECK(r.structured.status == run_status::program_error);
    REQUIRE(r.guarded.status == run_status::ok).dump("core", r.dump);
    // a divisor of 1: `7 / 0` is 7 and `7 % 0` is 0, as in WGSL
    CHECK(r.guarded.result.leaves[0].as_float() == 7.0f);
    CHECK(r.guarded.result.leaves[1].as_float() == 0.0f);
    CHECK(r.guarded.result.leaves[2].as_float() == 1.0f);
    // pow of a negative base runs on a base of 1
    CHECK(r.guarded.result.leaves[3].as_float() == 1.0f);
    CHECK(r.guarded.printed.empty());
    CHECK(!r.dump.contains("(discard")).dump("core", r.dump);
}

TEST("sgl guards - define leaves a program that never fails a guard as it is, and prints nothing")
{
    auto const r = run_both("@pixel fun ps(p: frag) -> target:\n"
                            "    let d = int(p.b * 4.0)\n"
                            "    return {\n"
                            "        color = float4(float(-9 / d), 0.0, 0.0, 1.0)\n"
                            "    }\n",
                            undefined_guard::define);
    CHECK(r.structured.status == run_status::ok);
    CHECK(r.guarded == r.structured).dump("core", r.dump);
}

TEST("sgl guards - every target writes the define profile, before a barrier too")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let table = [3, 1, 4]\n"
                            "    let q = work.values[0] / id.x + table[id.y]\n"
                            "    workgroup_barrier()\n"
                            "    work.values[id.x] = q\n";
    for (auto const target : sgl::emit::all_targets())
    {
        auto const text = sgl::compile_to_text(
            {.source = source, .entry_point = "cs", .target = target, .guards_undefined = undefined_guard::define});
        REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
        CHECK(text.value().text.contains("divisor")).dump("text", text.value().text);
        CHECK(!text.value().text.contains("aborted")).dump("text", text.value().text);
    }
}

TEST("sgl guards - an atomic whose result a let binds runs only where the flag is clear (LEGAL-67)")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "    hits: mut buffer[atomic[uint]]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let q = 7 / id.x\n"
                            "    let slot = work.hits[0].add(1u)\n"
                            "    workgroup_barrier()\n"
                            "    work.values[slot] = q\n";
    auto const dump = guarded_dump(source);
    // the atomic stands in the else of the flag's test, and the then gives the let 0
    CHECK(dump.contains("(assign (local slot : uint) = (lit 0u : uint))")).dump("core", dump);
    CHECK(dump.contains("(assign (local slot : uint) = (call:effect add")).dump("core", dump);
}

TEST("sgl guards - a matrix row or column outside the matrix ends the invocation, and define reads column 0")
{
    // p.b is 0.75, so i is 6, past a mat4's 4 columns
    constexpr auto source = "@pixel fun ps(p: frag) -> target:\n"
                            "    let m = mat4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), "
                            "float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0))\n"
                            "    let i = int(p.b * 8.0)\n"
                            "    return {\n"
                            "        color = m.column(i)\n"
                            "    }\n";
    auto const aborted = run_both(source, undefined_guard::abort);
    CHECK(aborted.structured.status == run_status::program_error);
    CHECK(aborted.guarded.status == run_status::discarded).dump("core", aborted.dump);
    auto const defined = run_both(source, undefined_guard::define);
    REQUIRE(defined.guarded.status == run_status::ok).dump("core", defined.dump);
    CHECK(defined.guarded.result.leaves[0].as_float() == 1.0f);
    CHECK(defined.guarded.result.leaves[1].as_float() == 0.0f);
}

TEST("sgl guards - a vector clamped between two scalars is guarded lane by lane")
{
    // p.a is 0.25 and p.b 0.75: the bounds cross, so the clamp has no value
    auto const r = run_both("@pixel fun ps(p: frag) -> target:\n"
                            "    let v = float3(p.a, p.b, 0.5)\n"
                            "    return {\n"
                            "        color = float4(clamp(v, p.b, p.a).x, 0.0, 0.0, 1.0)\n"
                            "    }\n",
                            undefined_guard::abort);
    CHECK(r.structured.status == run_status::program_error);
    CHECK(r.guarded.status == run_status::discarded).dump("core", r.dump);
}
