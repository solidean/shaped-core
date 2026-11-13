#include "flat-test-support.hh"

#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using namespace sgl::check;

// LEGAL-65 and LEGAL-66 over a binding array of buffers, and LEGAL-68 over memory that holds nothing.

namespace
{
symbol_id binding_named(checked_module const& m, cc::string_view name)
{
    for (auto i = isize(0); i < m.symbols.size(); ++i)
        if (m.symbols[i].kind == symbol_kind::binding && m.symbols[i].name == name)
            return symbol_id(i);
    return symbol_id::none;
}

/// Where the module has `parts`, its first element holds `first` ints from 10 up, and its second `second` from 20 up;
/// where it has `solid`, the same two sizes of `pair` and of `bytes`, whose scalars are their position.
run_inputs inputs_of(checked_module const& m, int first, int second)
{
    auto inputs = test_inputs(m);
    if (auto const parts = binding_named(m, "parts"); is_valid(parts))
    {
        auto a = buffer_contents{.binding = parts, .member = 0, .element = 0};
        auto b = buffer_contents{.binding = parts, .member = 0, .element = 1};
        for (auto i = 0; i < first; ++i)
            a.leaves.push_back(scalar::of(10 + i));
        for (auto i = 0; i < second; ++i)
            b.leaves.push_back(scalar::of(20 + i));
        inputs.buffers.push_back(cc::move(a));
        inputs.buffers.push_back(cc::move(b));
    }
    if (auto const solid = binding_named(m, "solid"); is_valid(solid))
    {
        auto pairs = buffer_contents{.binding = solid, .member = 0};
        auto raw = buffer_contents{.binding = solid, .member = 1};
        auto atomics = buffer_contents{.binding = solid, .member = 2};
        for (auto i = 0; i < 2 * first; ++i)
            pairs.leaves.push_back(scalar::of(i));
        for (auto i = 0; i < second; ++i)
            raw.leaves.push_back(scalar::of_uint(u32(i)));
        for (auto i = 0; i < first; ++i)
            atomics.leaves.push_back(scalar::of(i));
        inputs.buffers.push_back(cc::move(pairs));
        inputs.buffers.push_back(cc::move(raw));
        inputs.buffers.push_back(cc::move(atomics));
    }
    return inputs;
}

struct runs
{
    outcome structured;
    outcome guarded;
    flat_entry_point core;
    cc::string dump;
};

constexpr cc::string_view k_edges = "require binding_arrays\n"
                                    "\n"
                                    "struct frag:\n"
                                    "    a: float\n"
                                    "    b: float\n"
                                    "\n"
                                    "@pixel struct target:\n"
                                    "    color: float4\n"
                                    "\n";

constexpr cc::string_view k_parts = "binding parts:\n"
                                    "    chunks: mut buffer[int][2]\n"
                                    "\n";

constexpr cc::string_view k_solid = "struct pair:\n"
                                    "    x: int\n"
                                    "    y: int\n"
                                    "\n"
                                    "binding solid:\n"
                                    "    pairs: mut buffer[pair]\n"
                                    "    raw: mut bytes\n"
                                    "    counts: mut buffer[atomic[int]]\n"
                                    "\n";

runs run_both(cc::string_view body, undefined_guard guard, int first = 4, int second = 8)
{
    auto const checked = check_sources(read_prelude(), cc::string(k_edges) + body);
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& m = checked.module;
    auto const& e = m.entry_points[0];
    auto result = runs{.structured = interpret(m, e, inputs_of(m, first, second))};
    result.core = legalize(m, e, {.guards_undefined = guard});
    CHECK(!find_core_violation(m, result.core).has_value()).dump("core", dump_entry_point(m, result.core));
    result.dump = dump_entry_point(m, result.core);
    result.guarded = interpret(m, result.core, inputs_of(m, first, second));
    return result;
}

/// `p.a` is 0.25 and `p.b` 0.75, so `which` is 1 and `i` 6, inside the second element's 8 and past the first's 4.
cc::string reads_element(cc::string_view which)
{
    return cc::format("{}@pixel fun ps(p: frag){{parts}} -> target:\n"
                      "    let which = {}\n"
                      "    let i = int(p.b * 8.0)\n"
                      "    return {{\n"
                      "        color = float4(float(parts.chunks[nonuniform which][i]), 0.0, 0.0, 1.0)\n"
                      "    }}\n",
                      k_parts, which);
}
} // namespace

TEST("sgl - the length of an element of a binding array is that element's (CHK-460)")
{
    auto const r
        = run_both(cc::string(k_parts)
                       + "@pixel fun ps(p: frag){parts} -> target:\n"
                         "    let which = int(p.b * 1.5)\n"
                         "    return {\n"
                         "        color = float4(float(parts.chunks[nonuniform which].length), 0.0, 0.0, 1.0)\n"
                         "    }\n",
                   undefined_guard::report);
    REQUIRE(r.structured.status == run_status::ok);
    CHECK(r.structured.result.leaves[0].as_float() == 8.0f);
    CHECK(r.guarded == r.structured).dump("core", r.dump);
}

TEST("sgl guards - a binding array's own index ends the invocation where it is outside the array (LEGAL-65)")
{
    auto const r = run_both(reads_element("int(p.b * 4.0)"), undefined_guard::report);
    CHECK(r.structured.status == run_status::program_error);
    CHECK(r.guarded.status == run_status::discarded).dump("core", r.dump);
    REQUIRE(r.core.messages.size() >= 1);
    CHECK(r.core.messages[0].pieces[1] == " is outside 0 ..< 2");
    REQUIRE(r.guarded.printed.size() == 1);
    CHECK(r.guarded.printed[0].values[0].leaves[0].as_int() == 3);
}

TEST("sgl guards - a buffer index past an element of a binding array is measured against that element (LEGAL-66)")
{
    auto const fits = run_both(reads_element("int(p.b * 1.5)"), undefined_guard::report);
    REQUIRE(fits.structured.status == run_status::ok);
    CHECK(fits.structured.result.leaves[0].as_float() == 26.0f);
    CHECK(fits.guarded == fits.structured).dump("core", fits.dump);

    auto const past = run_both(reads_element("int(p.a * 2.0)"), undefined_guard::report);
    CHECK(past.structured.status == run_status::program_error);
    CHECK(past.guarded.status == run_status::discarded).dump("core", past.dump);
    REQUIRE(past.guarded.printed.size() == 1);
    CHECK(past.guarded.printed[0].values[0].leaves[0].as_int() == 6);
    CHECK(past.guarded.printed[0].values[1].leaves[0].as_uint() == 4u);
}

TEST("sgl guards - a store through a binding array behaves as it does unguarded, and evaluates its index once")
{
    for (auto const guard : {undefined_guard::abort, undefined_guard::report, undefined_guard::define})
    {
        auto const r = run_both(cc::string(k_parts)
                                    + "@pixel fun ps(p: frag){parts} -> target:\n"
                                      "    let which = int(p.b * 1.5)\n"
                                      "    parts.chunks[nonuniform which][2] = parts.chunks[nonuniform which][3] + 1\n"
                                      "    return {\n"
                                      "        color = float4(float(parts.chunks[1][2]), 0.0, 0.0, 1.0)\n"
                                      "    }\n",
                                guard);
        REQUIRE(r.structured.status == run_status::ok);
        CHECK(r.structured.result.leaves[0].as_float() == 24.0f);
        CHECK(r.guarded == r.structured).dump("core", r.dump);
    }
}

TEST("sgl guards - define takes an index of 0 where a binding array's own index fails (LEGAL-68)")
{
    auto const r = run_both(reads_element("int(p.b * 4.0)"), undefined_guard::define);
    REQUIRE(r.guarded.status == run_status::ok);
    // element 0 holds 4, so the index of 6 fails too, and element 0's first int is read
    CHECK(r.guarded.result.leaves[0].as_float() == 10.0f);
    CHECK(r.guarded.printed.empty());
}

TEST("sgl guards - define leaves memory that holds nothing untouched: a read is zero, and a store is dropped "
     "(LEGAL-68)")
{
    constexpr auto body = "@pixel fun ps(p: frag){solid} -> target:\n"
                          "    let i = int(p.a * 4.0)\n"
                          "    let held = solid.pairs[i]\n"
                          "    solid.pairs[i] = held\n"
                          "    let word = solid.raw.load(0u)\n"
                          "    solid.raw.store(0u, 9u)\n"
                          "    let before = solid.counts[i].add(3)\n"
                          "    solid.counts[i].store(4)\n"
                          "    return {\n"
                          "        color = float4(float(held.x + held.y), float(word), float(before), 1.0)\n"
                          "    }\n";
    auto const empty = run_both(cc::string(k_solid) + body, undefined_guard::define, 0, 0);
    REQUIRE(empty.guarded.status == run_status::ok).dump("core", empty.dump);
    CHECK(empty.guarded.result.leaves[0].as_float() == 0.0f);
    CHECK(empty.guarded.result.leaves[1].as_float() == 0.0f);
    CHECK(empty.guarded.result.leaves[2].as_float() == 0.0f);

    // and memory that holds something behaves as it does unguarded, the guards having no effect of their own
    auto const held = run_both(cc::string(k_solid) + body, undefined_guard::define, 4, 8);
    REQUIRE(held.structured.status == run_status::ok);
    CHECK(held.guarded == held.structured).dump("core", held.dump);
}

TEST("sgl guards - define runs a load of more words than the memory holds on no word of it (LEGAL-68)")
{
    auto const r = run_both(cc::string(k_solid)
                                + "@pixel fun ps(p: frag){solid} -> target:\n"
                                  "    let words = solid.raw.load2(0u)\n"
                                  "    return {\n"
                                  "        color = float4(float(words.x), float(words.y), 0.0, 1.0)\n"
                                  "    }\n",
                            undefined_guard::define, 4, 1);
    CHECK(r.structured.status == run_status::program_error);
    REQUIRE(r.guarded.status == run_status::ok).dump("core", r.dump);
    CHECK(r.guarded.result.leaves[0].as_float() == 0.0f);
    CHECK(r.guarded.result.leaves[1].as_float() == 0.0f);
}

TEST("sgl emit - an element's length keeps its mark in the helper, and a zero is spelled per type (EMIT-195, LEGAL-68)")
{
    auto const source
        = cc::string("require binding_arrays\n"
                     "\n"
                     "struct pair:\n"
                     "    x: int\n"
                     "    y: int\n"
                     "\n"
                     "binding work:\n"
                     "    pairs: buffer[pair]\n"
                     "    values: mut buffer[int]\n"
                     "    views: buffer[int][4]\n"
                     "    raw: bytes[4]\n"
                     "\n"
                     "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                     "    work.values[id.x] = work.views[nonuniform (id.x % 4)][id.x] + work.pairs[id.x].y\n"
                     "    work.values[0] = int(work.views[0].length + work.raw[nonuniform (id.x % 4)].load(0u))\n");
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const text = sgl::compile_to_text(
            {.source = source, .entry_point = "cs", .target = t, .guards_undefined = undefined_guard::define});
        REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
        if (t == sgl::emit::target::hlsl_dx12)
            CHECK(text.value().text.contains("[NonUniformResourceIndex(i)].GetDimensions(n, stride);"))
                .dump("text", text.value().text);
        else // asked per element, at uniform indices (debt-sgl-vulkan-element-length-asked-at-uniform-indices)
            CHECK(text.value().text.contains("[3].GetDimensions(k, stride);\n    n = i == 3u ? k : n;"))
                .dump("text", text.value().text);
        CHECK(text.value().text.contains(")0;")).dump("text", text.value().text);
    }
}

TEST("sgl emit - metal bounds a binding array's own index by its size, and reads no length (LEGAL-65)")
{
    constexpr auto source = "require binding_arrays\n"
                            "\n"
                            "binding work:\n"
                            "    written: mut buffer[int]\n"
                            "    views: buffer[int][4]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    work.written[0] = work.views[nonuniform id.x][0]\n";
    auto const text = sgl::compile_to_text({.source = source,
                                            .entry_point = "cs",
                                            .target = sgl::emit::target::msl,
                                            .guards_undefined = undefined_guard::abort});
    REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
}

TEST("sgl guards - an unbounded array's index is bounded by its length, whether or not its code reads it (LEGAL-65, "
     "CHK-491)")
{
    constexpr auto program = "require binding_arrays, unbounded_binding_arrays\n"
                             "\n"
                             "binding work:\n"
                             "    written: mut buffer[int]\n"
                             "    views: buffer[int][]\n"
                             "\n"
                             "@compute(64) fun cs(@thread_id id: int3){{work}}:\n"
                             "    work.written[id.x] = work.views[nonuniform id.x][0]{}\n";
    auto const dump_of = [&](cc::string_view tail)
    {
        auto const checked = check_sources(read_prelude(), cc::format(program, tail));
        REQUIRE(reports_of(checked) == "");
        auto const core = legalize(checked.module, checked.module.entry_points[0],
                                   {.guards_undefined = undefined_guard::report, .guards_buffers = true});
        CHECK(!find_core_violation(checked.module, core).has_value());
        CHECK(sgl::compile_to_text({.source = cc::format(program, tail),
                                    .entry_point = "cs",
                                    .target = sgl::emit::target::hlsl_dx12,
                                    .guards_undefined = undefined_guard::report})
                  .has_value());
        auto pieces = cc::string();
        for (auto const& message : core.messages)
            for (auto const& piece : message.pieces)
                pieces += piece;
        return pieces;
    };
    CHECK(dump_of("").contains("outside a binding array"));
    CHECK(dump_of(" + int(work.views.length)").contains("outside a binding array"));
}
