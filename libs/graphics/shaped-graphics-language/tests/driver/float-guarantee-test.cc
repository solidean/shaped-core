#include "../check/check-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::check::float_level;
using sgl::emit::target;

// CHK-435 and EMIT-185: a function states the float guarantee it needs, an entry point keeps the strictest it reaches,
// and a target that cannot keep it refuses, unless the host weakened it on WebGPU.

namespace
{
constexpr cc::string_view k_layered = "binding work:\n"
                                      "    values: mut buffer[float]\n"
                                      "\n"
                                      "@float(.specials) fun nan_aware(x: float) -> float:\n"
                                      "    return if x != x => 0.0 else x\n"
                                      "\n"
                                      "@float(.precise) fun ordered(a: float, b: float) -> float:\n"
                                      "    return (a + b) - a\n"
                                      "\n"
                                      "fun plain(x: float) -> float:\n"
                                      "    return x * 2.0\n"
                                      "\n"
                                      "@compute(64) fun both(@thread_id id: int3){work}:\n"
                                      "    let v = work.values[id.x]\n"
                                      "    work.values[id.x] = ordered(nan_aware(v), plain(v))\n"
                                      "\n"
                                      "@compute(64) fun loose(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = plain(work.values[id.x])\n"
                                      "\n"
                                      "@float(.ieee) @compute(64) fun exact(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = plain(work.values[id.x])\n";

cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view entry,
                                                     target t,
                                                     bool weakens = false,
                                                     float_level floor = float_level::fast)
{
    return sgl::compile_to_text({.source = k_layered,
                                 .source_name = "float.sgl",
                                 .entry_point = entry,
                                 .target = t,
                                 .weakens_float = weakens,
                                 .float_floor = floor});
}

/// The entry point named `name` of `d`, which the test requires.
sgl::described_entry_point const& entry_named(sgl::module_description const& d, cc::string_view name)
{
    for (auto const& e : d.entry_points)
        if (e.name == name)
            return e;
    REQUIRE(false).dump("entry", name);
    return d.entry_points[0];
}
} // namespace

TEST("sgl float - a request's floor raises every entry point to at least its level, as if each stated it")
{
    // below the floor: raised, and no function named, since none asked
    auto const loose = compiled("loose", target::hlsl_dx12, false, float_level::precise);
    REQUIRE(loose.has_value()).dump("error", loose.has_error() ? loose.error() : cc::string());
    CHECK(loose.value().float_guarantee == float_level::precise);
    CHECK(loose.value().float_raised_by.empty());
    CHECK(loose.value().is_float_raised_by_floor);

    // at or above it: the entry point's own level, and the function that stated it
    auto const both = compiled("both", target::hlsl_dx12, false, float_level::specials);
    REQUIRE(both.has_value());
    CHECK(both.value().float_guarantee == float_level::precise);
    CHECK(both.value().float_raised_by == "ordered");
    CHECK(!both.value().is_float_raised_by_floor);
    auto const level = compiled("both", target::hlsl_dx12, false, float_level::precise);
    REQUIRE(level.has_value());
    CHECK(level.value().float_raised_by == "ordered");
    CHECK(!level.value().is_float_raised_by_floor);
    auto const exact = compiled("exact", target::hlsl_dx12, false, float_level::precise);
    REQUIRE(exact.has_value());
    CHECK(exact.value().float_guarantee == float_level::ieee);
    CHECK(exact.value().float_raised_by == "exact");

    // a target keeps a raised level as it keeps a stated one: vulkan through the features, metal through safe math
    auto const vulkan = compiled("loose", target::hlsl_vulkan, false, float_level::ieee);
    REQUIRE(vulkan.has_value());
    CHECK(vulkan.value().features.has(sgl::check::feature::float_specials));
    CHECK(vulkan.value().features.has(sgl::check::feature::float_denormals));
    auto const metal = compiled("loose", target::msl, false, float_level::specials);
    REQUIRE(metal.has_value());
    CHECK(metal.value().text.contains("#pragma METAL fp math_mode(safe)\n")).dump("text", metal.value().text);
}

TEST("sgl float - a floor a target cannot keep is refused naming the floor, and WebGPU weakens it as a stated one")
{
    auto const refused = compiled("loose", target::wgsl, false, float_level::specials);
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("float-guarantee-not-kept")).dump("error", refused.error());
    CHECK(refused.error().contains("the request's float floor")).dump("error", refused.error());
    CHECK(!refused.error().contains("''")).dump("error", refused.error());
    // metal keeps up to precise, so a floor of ieee is refused there too
    auto const metal = compiled("loose", target::msl, false, float_level::ieee);
    REQUIRE(metal.has_error());
    CHECK(metal.error().contains("the request's float floor")).dump("error", metal.error());
    // where a function asks more than the floor, the refusal names the function
    auto const stated = compiled("both", target::wgsl, false, float_level::specials);
    REQUIRE(stated.has_error());
    CHECK(stated.error().contains("'ordered'")).dump("error", stated.error());

    auto const weakened = compiled("loose", target::wgsl, true, float_level::precise);
    REQUIRE(weakened.has_value()).dump("error", weakened.has_error() ? weakened.error() : cc::string());
    CHECK(weakened.value().float_guarantee == float_level::fast);
    CHECK(!weakened.value().is_float_raised_by_floor);
    REQUIRE(weakened.value().warnings.size() == 1);
    CHECK(weakened.value().warnings[0].contains("float-guarantee-weakened"));
    CHECK(weakened.value().warnings[0].contains("the request's float floor")).dump("warning", weakened.value().warnings[0]);
    // with no floor and nothing stated, nothing was weakened
    auto const plain = compiled("loose", target::wgsl, true);
    REQUIRE(plain.has_value());
    CHECK(plain.value().warnings.empty());
}

TEST("sgl float - code a floor raises above fast does not warn of a NaN test at fast (CHK-436)")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let v = work.values[id.x]\n"
                            "    work.values[id.x] = if v != v => 0.0 else v\n";
    auto const at_fast = sgl::compile_to_text({.source = source, .entry_point = "cs"});
    REQUIRE(at_fast.has_value());
    auto warned = false;
    for (auto const& w : at_fast.value().warnings)
        warned = warned || w.contains("nan-test-at-fast");
    CHECK(warned);
    auto const floored
        = sgl::compile_to_text({.source = source, .entry_point = "cs", .float_floor = float_level::specials});
    REQUIRE(floored.has_value());
    for (auto const& w : floored.value().warnings)
        CHECK(!w.contains("nan-test-at-fast")).dump("warning", w);

    // WebGPU weakening the floor back to fast compiles the test at fast again, so it warns, before the weakening
    auto const weakened = sgl::compile_to_text({.source = source,
                                                .entry_point = "cs",
                                                .target = target::wgsl,
                                                .weakens_float = true,
                                                .float_floor = float_level::specials});
    REQUIRE(weakened.has_value()).dump("error", weakened.has_error() ? weakened.error() : cc::string());
    REQUIRE(weakened.value().warnings.size() == 2);
    CHECK(weakened.value().warnings[0].contains("nan-test-at-fast")).dump("warning", weakened.value().warnings[0]);
    CHECK(weakened.value().warnings[1].contains("float-guarantee-weakened")).dump("warning", weakened.value().warnings[1]);
}

TEST("sgl float - a polyfill's fused level warns of a weakening only where one happened")
{
    // the geometry stage states nothing, and the vertex stage its compute half runs raises the fused entry point
    constexpr auto source = "require geometry_shader\n"
                            "\n"
                            "@vertex struct corner_in:\n"
                            "    position: float2\n"
                            "\n"
                            "struct staged:\n"
                            "    @position position: hpos4\n"
                            "    color: float4\n"
                            "\n"
                            "@float(.specials) @vertex fun corner_vs(v: corner_in) -> staged:\n"
                            "    return {position = hpos4(v.position.x, v.position.y, 0.0, 1.0), color = float4(1.0)}\n"
                            "\n"
                            "@geometry(max_vertices = 3)\n"
                            "fun tri_gs(tri: staged[3], stream: mut triangle_stream[staged]):\n"
                            "    for i in 0 ..< 3:\n"
                            "        stream.emit(tri[i])\n";
    auto const half = [&](target t, bool weakens)
    {
        return sgl::compile_to_text({.source = source,
                                     .source_name = "fused.sgl",
                                     .entry_point = "tri_gs",
                                     .target = t,
                                     .weakens_float = weakens,
                                     .mesh = sgl::emit::mesh_form::compute,
                                     .polyfill_vertex = "corner_vs"});
    };
    // kept: the fused level, and no weakening warned of, though the geometry stage alone is at fast
    auto const kept = half(target::hlsl_dx12, false);
    REQUIRE(kept.has_value()).dump("error", kept.has_error() ? kept.error() : cc::string());
    CHECK(kept.value().float_guarantee == float_level::specials);
    CHECK(kept.value().float_raised_by == "corner_vs");
    for (auto const& w : kept.value().warnings)
        CHECK(!w.contains("float-guarantee-weakened")).dump("warning", w);
    // weakened: warned of, naming the vertex stage, though the geometry stage alone is at the fast it runs at
    auto const weakened = half(target::wgsl, true);
    REQUIRE(weakened.has_value()).dump("error", weakened.has_error() ? weakened.error() : cc::string());
    CHECK(weakened.value().float_guarantee == float_level::fast);
    auto warned = 0;
    for (auto const& w : weakened.value().warnings)
        if (w.contains("float-guarantee-weakened"))
        {
            ++warned;
            CHECK(w.contains("'corner_vs'")).dump("warning", w);
            CHECK(w.contains("'tri_gs'")).dump("warning", w);
        }
    CHECK(warned == 1);
}

TEST("sgl float - describe reports the level a floor raises an entry point to, and that the floor raised it")
{
    auto const plain = sgl::describe({.source = k_layered, .source_name = "float.sgl"});
    REQUIRE(plain.has_value()).dump("error", plain.has_error() ? plain.error() : cc::string());
    CHECK(entry_named(plain.value(), "loose").float_guarantee == float_level::fast);
    CHECK(!entry_named(plain.value(), "loose").is_float_raised_by_floor);

    auto const floored
        = sgl::describe({.source = k_layered, .source_name = "float.sgl", .float_floor = float_level::precise});
    REQUIRE(floored.has_value());
    auto const& loose = entry_named(floored.value(), "loose");
    CHECK(loose.float_guarantee == float_level::precise);
    CHECK(loose.float_raised_by.empty());
    CHECK(loose.is_float_raised_by_floor);
    auto const& both = entry_named(floored.value(), "both");
    CHECK(both.float_guarantee == float_level::precise);
    CHECK(both.float_raised_by == "ordered");
    CHECK(!both.is_float_raised_by_floor);
    auto const& exact = entry_named(floored.value(), "exact");
    CHECK(exact.float_guarantee == float_level::ieee);
    CHECK(!exact.is_float_raised_by_floor);

    // describe and the text agree on every entry point
    for (auto const& e : floored.value().entry_points)
    {
        auto const text = compiled(e.name, target::hlsl_dx12, false, float_level::precise);
        REQUIRE(text.has_value());
        CHECK(text.value().float_guarantee == e.float_guarantee).dump("entry", e.name);
        CHECK(text.value().float_raised_by == e.float_raised_by).dump("entry", e.name);
        CHECK(text.value().is_float_raised_by_floor == e.is_float_raised_by_floor).dump("entry", e.name);
    }
}

TEST("sgl float - an entry point keeps the strictest @float of what it reaches, and names the function that raised it")
{
    auto const loose = compiled("loose", target::hlsl_dx12);
    REQUIRE(loose.has_value()).dump("error", loose.has_error() ? loose.error() : cc::string());
    CHECK(loose.value().float_guarantee == float_level::fast);
    CHECK(loose.value().float_raised_by.empty());

    auto const both = compiled("both", target::hlsl_dx12);
    REQUIRE(both.has_value()).dump("error", both.has_error() ? both.error() : cc::string());
    CHECK(both.value().float_guarantee == float_level::precise);
    CHECK(both.value().float_raised_by == "ordered");

    auto const exact = compiled("exact", target::hlsl_dx12);
    REQUIRE(exact.has_value());
    CHECK(exact.value().float_guarantee == float_level::ieee);
    CHECK(exact.value().float_raised_by == "exact");
}

TEST("sgl float - each target keeps what it can, and refuses more by name")
{
    CHECK(compiled("both", target::msl).has_value());
    CHECK(compiled("loose", target::wgsl).has_value());
    CHECK(compiled("loose", target::hlsl_vulkan).has_value());

    auto const refused = compiled("both", target::wgsl);
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("float-guarantee-not-kept")).dump("error", refused.error());
    CHECK(refused.error().contains("'ordered'")).dump("error", refused.error());

    // vulkan keeps every level through execution modes the device must have, so the entry point needs their features
    auto const precise = compiled("both", target::hlsl_vulkan);
    REQUIRE(precise.has_value()).dump("error", precise.has_error() ? precise.error() : cc::string());
    CHECK(precise.value().features.has(sgl::check::feature::float_specials));
    CHECK(!precise.value().features.has(sgl::check::feature::float_denormals));
    auto const ieee = compiled("exact", target::hlsl_vulkan);
    REQUIRE(ieee.has_value());
    CHECK(ieee.value().features.has(sgl::check::feature::float_specials));
    CHECK(ieee.value().features.has(sgl::check::feature::float_denormals));
    CHECK(!compiled("loose", target::hlsl_vulkan).value().features.has(sgl::check::feature::float_specials));
    // dx12 keeps them on every device, so it needs no feature
    CHECK(!compiled("exact", target::hlsl_dx12).value().features.has(sgl::check::feature::float_specials));

    // metal's safe math is not known to keep denormals
    auto const metal = compiled("exact", target::msl);
    REQUIRE(metal.has_error());
    CHECK(metal.error().contains("float-guarantee-not-kept"));
}

TEST("sgl float - a host may weaken a guarantee on WebGPU alone, which compiles at fast and warns")
{
    auto const weakened = compiled("both", target::wgsl, true);
    REQUIRE(weakened.has_value()).dump("error", weakened.has_error() ? weakened.error() : cc::string());
    CHECK(weakened.value().float_guarantee == float_level::fast);
    REQUIRE(weakened.value().warnings.size() == 1);
    CHECK(weakened.value().warnings[0].contains("float-guarantee-weakened"));

    // the other targets take no weakening: what a shader needs is refused rather than dropped there
    CHECK(compiled("exact", target::msl, true).has_error());
}

TEST("sgl float - @float takes one of its four levels")
{
    constexpr auto bad = "@float(.strict) fun f(x: float) -> float:\n"
                         "    return x\n"
                         "\n"
                         "@compute(1) fun cs():\n"
                         "    let unused = f(1.0)\n";
    CHECK(reports_for(bad).contains("invalid-attribute-arguments")).dump("reports", reports_for(bad));
}

TEST("sgl float - a float compared with itself warns at fast and not at specials, and is_nan never does (CHK-436)")
{
    constexpr auto loose = "binding work:\n"
                           "    values: mut buffer[float]\n"
                           "\n"
                           "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                           "    let v = work.values[id.x]\n"
                           "    work.values[id.x] = if v != v => 0.0 else v\n";
    auto const reports = reports_for(loose);
    CHECK(reports.contains("nan-test-at-fast")).dump("reports", reports);

    constexpr auto bits = "binding work:\n"
                          "    values: mut buffer[float]\n"
                          "\n"
                          "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                          "    let v = work.values[id.x]\n"
                          "    work.values[id.x] = if is_nan(v) => 0.0 else v\n";
    CHECK(!reports_for(bits).contains("nan-test-at-fast")).dump("reports", reports_for(bits));

    constexpr auto kept = "binding work:\n"
                          "    values: mut buffer[float]\n"
                          "\n"
                          "@float(.specials) @compute(64) fun cs(@thread_id id: int3){work}:\n"
                          "    let v = work.values[id.x]\n"
                          "    work.values[id.x] = if is_nan(v) or v != v => 0.0 else v\n";
    CHECK(!reports_for(kept).contains("nan-test-at-fast")).dump("reports", reports_for(kept));
}

// EMIT-262: Metal's math mode is the text's own, so the entry point's guarantee reaches the compile whatever it was handed.
TEST("sgl float - metal's text states its entry point's math mode, fast for no guarantee and safe for any")
{
    auto const fast_pragma = cc::string_view("#pragma METAL fp math_mode(fast)\n");
    auto const safe_pragma = cc::string_view("#pragma METAL fp math_mode(safe)\n");
    auto const include = cc::string_view("#include <metal_stdlib>");

    auto const loose = compiled("loose", target::msl);
    REQUIRE(loose.has_value()).dump("error", loose.has_error() ? loose.error() : cc::string());
    auto const& loose_text = loose.value().text;
    CHECK(loose_text.contains(fast_pragma)).dump("text", loose_text);
    CHECK(!loose_text.contains(safe_pragma));
    CHECK(loose_text.find(fast_pragma) < loose_text.find(include));

    auto const both = compiled("both", target::msl);
    REQUIRE(both.has_value()).dump("error", both.has_error() ? both.error() : cc::string());
    auto const& both_text = both.value().text;
    CHECK(both_text.contains(safe_pragma)).dump("text", both_text);
    CHECK(!both_text.contains(fast_pragma));
    CHECK(both_text.find(safe_pragma) < both_text.find(include));

    // the other targets state no math mode in their text: DXC and Vulkan take it from the compile
    auto const hlsl = compiled("loose", target::hlsl_dx12);
    REQUIRE(hlsl.has_value());
    CHECK(!hlsl.value().text.contains("math_mode"));
}
