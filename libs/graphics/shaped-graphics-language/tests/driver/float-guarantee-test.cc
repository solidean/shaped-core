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

namespace
{
/// A matrix product of a matrix product, a dot and a smoothstep, at `.fast` and at `.precise`.
constexpr cc::string_view k_products
    = "binding work:\n"
      "    values: mut buffer[float4]\n"
      "\n"
      "fun shaped(id: int3){work} -> float4:\n"
      "    let m = mat4(work.values[0], work.values[1], work.values[2], work.values[3])\n"
      "    let v = work.values[id.x]\n"
      "    let p = (m * m) * v\n"
      "    return p + float4(dot(v, p), smoothstep(v.x, v.y, v.z), 0.0, 0.0)\n"
      "\n"
      "@compute(64) fun loose(@thread_id id: int3){work}:\n"
      "    work.values[id.x] = shaped(id)\n"
      "\n"
      "@float(.precise) @compute(64) fun strict(@thread_id id: int3){work}:\n"
      "    work.values[id.x] = shaped(id)\n";

/// How often `text` calls `name` itself, not a longer name that ends in it.
isize bare_calls(cc::string_view text, cc::string_view name)
{
    auto count = isize(0);
    auto const call = cc::string(name) + "(";
    for (auto at = text.find(call); at >= 0; at = text.find(call, at + 1))
    {
        auto const before = at == 0 ? ' ' : text[at - 1];
        auto const is_part = before == '_' || (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z')
                          || (before >= '0' && before <= '9');
        count += is_part ? 0 : 1;
    }
    return count;
}
} // namespace

TEST("sgl float - a text at .precise writes its products and smoothstep as helpers in the fixed order (EMIT-281)")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
    {
        auto const of = [&](cc::string_view entry)
        {
            auto const e = sgl::compile_to_text(
                {.source = k_products, .source_name = "products.sgl", .entry_point = entry, .target = t});
            REQUIRE(e.has_value())
                .dump("target", sgl::emit::to_string(t))
                .dump("error", e.has_error() ? e.error() : cc::string());
            return e.value().text;
        };
        auto const loose = of("loose");
        auto const strict = of("strict");
        auto const is_hlsl = t != target::msl;

        // .fast keeps the target's own: HLSL's `mul`, MSL's `*`, and every target's `dot` and `smoothstep`
        CHECK(bare_calls(loose, "mul") == (is_hlsl ? 2 : 0)).dump("text", loose);
        CHECK(bare_calls(loose, "dot") == 1).dump("text", loose);
        CHECK(bare_calls(loose, "smoothstep") == 1).dump("text", loose);
        CHECK(!loose.contains("sgl_ordered_"));

        // .precise calls none of them, and spells each sum out, left to right
        CHECK(bare_calls(strict, "mul") == 0).dump("text", strict);
        CHECK(bare_calls(strict, "dot") == 0).dump("text", strict);
        CHECK(bare_calls(strict, "smoothstep") == 0).dump("text", strict);
        CHECK(bare_calls(strict, "sgl_ordered_mul") == 2 + 2).dump("text", strict); // two declarations, two calls
        CHECK(strict.contains("float sgl_ordered_dot(float4 a, float4 b)\n{\n    return (((a[0] * b[0] + a[1] * b[1]) "
                              "+ a[2] * b[2]) + a[3] * b[3]);\n}\n"))
            .dump("text", strict);
        CHECK(strict.contains("return t * (t * ((float)3.0 - (float)2.0 * t));")).dump("text", strict);
        // row 1 of `m * v`: HLSL indexes a row, MSL a column
        CHECK(strict.contains(is_hlsl ? "(((m[1][0] * v[0] + m[1][1] * v[1]) + m[1][2] * v[2]) + m[1][3] * v[3])"
                                      : "(((m[0][1] * v[0] + m[1][1] * v[1]) + m[2][1] * v[2]) + m[3][1] * v[3])"))
            .dump("text", strict);
    }
}

namespace
{
/// The geometry and the blend that hide a dot or a sum, at `.fast` and at `.precise`.
constexpr cc::string_view k_geometry
    = "binding work:\n"
      "    values: mut buffer[float4]\n"
      "\n"
      "fun shaped(id: int3){work} -> float4:\n"
      "    let v = work.values[id.x]\n"
      "    let w = work.values[0]\n"
      "    let n = vec3(w.x, w.y, w.z)\n"
      "    let i = vec3(v.x, v.y, v.z)\n"
      "    let r = reflect(i, n) + refract(i, n, w.w) + cross(i, n)\n"
      "    let m = mix(v, w, v.x)\n"
      "    return m + normalize(v) + float4(r.x, r.y, r.z, length(v) + distance(v, w))\n"
      "\n"
      "@compute(64) fun loose(@thread_id id: int3){work}:\n"
      "    work.values[id.x] = shaped(id)\n"
      "\n"
      "@float(.precise) @compute(64) fun strict(@thread_id id: int3){work}:\n"
      "    work.values[id.x] = shaped(id)\n";
} // namespace

TEST("sgl float - a text at .precise writes the geometry and mix as helpers of their fixed forms (EMIT-281)")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
    {
        auto const of = [&](cc::string_view entry)
        {
            auto const e = sgl::compile_to_text(
                {.source = k_geometry, .source_name = "geometry.sgl", .entry_point = entry, .target = t});
            REQUIRE(e.has_value())
                .dump("target", sgl::emit::to_string(t))
                .dump("error", e.has_error() ? e.error() : cc::string());
            return e.value().text;
        };
        auto const loose = of("loose");
        auto const strict = of("strict");
        auto const blend = t == target::msl ? cc::string_view("mix") : cc::string_view("lerp");
        for (auto const name : {cc::string_view("length"), cc::string_view("distance"), cc::string_view("normalize"),
                                cc::string_view("reflect"), cc::string_view("refract"), cc::string_view("cross"), blend})
        {
            CHECK(bare_calls(loose, name) == 1).dump("call", name).dump("text", loose);
            CHECK(bare_calls(strict, name) == 0).dump("call", name).dump("text", strict);
            CHECK(bare_calls(strict, cc::format("sgl_ordered_{}", name == blend ? cc::string_view("mix") : name)) == 2)
                .dump("call", name)
                .dump("text", strict);
        }
        CHECK(!loose.contains("sgl_ordered_"));

        // each form as EMIT-281 states it
        CHECK(strict.contains("    return a + t * (b - a);\n")).dump("text", strict);
        CHECK(strict.contains("    float s = (float)1.0 / sqrt((((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * "
                              "v[3]));\n    return v * s;\n"))
            .dump("text", strict);
        CHECK(strict.contains("    float k = (float)2.0 * ((n[0] * i[0] + n[1] * i[1]) + n[2] * i[2]);\n"
                              "    return i - n * k;\n"))
            .dump("text", strict);
        CHECK(strict.contains("    float k = (float)1.0 - (eta * eta) * ((float)1.0 - d * d);\n"
                              "    return k >= (float)0.0 ? eta * i - (eta * d + sqrt(k)) * n : "))
            .dump("text", strict);
        CHECK(strict.contains("    float4 d = a - b;\n    return sqrt((((d[0] * d[0] + d[1] * d[1]) + d[2] * d[2]) + "
                              "d[3] * "
                              "d[3]));\n"))
            .dump("text", strict);
    }
}

namespace
{
/// Each matrix shape's product at `.precise`: mat3 and mat2 by a vector and by each other, and an affine3's three.
constexpr cc::string_view k_shapes = "binding work:\n"
                                     "    values: mut buffer[float4]\n"
                                     "\n"
                                     "@float(.precise) @compute(64) fun strict(@thread_id id: int3){work}:\n"
                                     "    let a = work.values[0]\n"
                                     "    let b = work.values[1]\n"
                                     "    let c = work.values[2]\n"
                                     "    let m3 = mat3(a.xyz, b.xyz, c.xyz)\n"
                                     "    let m2 = mat2(a.xy, b.xy)\n"
                                     "    let af = affine3.from_rows(a, b, c)\n"
                                     "    let s = (m3 * m3) * c.xyz\n"
                                     "    let t = (m2 * m2) * c.xy\n"
                                     "    let u = af * pos3(c.x, c.y, c.z)\n"
                                     "    work.values[id.x] = float4(s.x + t.x, u.x, u.y, u.z)\n";
} // namespace

TEST("sgl float - a text at .precise indexes each matrix shape's elements as its target holds them (EMIT-281)")
{
    for (auto const t : {target::hlsl_dx12, target::msl})
    {
        auto const e = sgl::compile_to_text(
            {.source = k_shapes, .source_name = "shapes.sgl", .entry_point = "strict", .target = t});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        auto const& text = e.value().text;
        auto const is_hlsl = t != target::msl;
        // row 1 of a mat3 times a vector: HLSL's m[row][column], MSL's m[column][row]
        CHECK(text.contains(is_hlsl ? "((m[1][0] * v[0] + m[1][1] * v[1]) + m[1][2] * v[2])"
                                    : "((m[0][1] * v[0] + m[1][1] * v[1]) + m[2][1] * v[2])"))
            .dump("text", text);
        // element (row 0, column 1) of a mat2 times a mat2
        CHECK(text.contains(is_hlsl ? "o[0][1] = (a[0][0] * b[0][1] + a[0][1] * b[1][1]);"
                                    : "o[1][0] = (a[0][0] * b[1][0] + a[1][0] * b[1][1]);"))
            .dump("text", text);
        // an affine3 is three rows of four in both, each row dotted with the widened position
        CHECK(text.contains("float3 sgl_ordered_mul(float3x4 m, float4 v)")).dump("text", text);
        CHECK(text.contains("(((m[2][0] * v[0] + m[2][1] * v[1]) + m[2][2] * v[2]) + m[2][3] * v[3])")).dump("text", text);
        CHECK(text.contains("sgl_ordered_mul(af, float4(float3(c.x, c.y, c.z), 1.0))")).dump("text", text);
    }
}

TEST("sgl float - a text at .precise spells a half vector's helpers in halves, every lane summed (EMIT-281)")
{
    constexpr cc::string_view source = "require shader_f16\n"
                                       "\n"
                                       "binding work:\n"
                                       "    values: mut buffer[float4]\n"
                                       "\n"
                                       "@float(.precise) @compute(64) fun strict(@thread_id id: int3){work}:\n"
                                       "    let v = work.values[id.x]\n"
                                       "    let h = half3(v.x as half, v.y as half, v.z as half)\n"
                                       "    let s = smoothstep(h, h + h, h)\n"
                                       "    work.values[id.x] = float4((dot(h, h) + length(h)) as float, s.x as float, "
                                       "0.0, 0.0)\n";
    for (auto const t : {target::hlsl_dx12, target::msl})
    {
        auto const e
            = sgl::compile_to_text({.source = source, .source_name = "half.sgl", .entry_point = "strict", .target = t});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        auto const& text = e.value().text;
        auto const half = t == target::msl ? cc::string_view("half") : cc::string_view("float16_t");
        CHECK(text.contains(cc::format("{0} sgl_ordered_dot({0}3 a, {0}3 b)\n{{\n    return ((a[0] * b[0] + a[1] * "
                                       "b[1]) "
                                       "+ a[2] * b[2]);\n}}\n",
                                       half)))
            .dump("text", text);
        CHECK(text.contains(cc::format("{0} sgl_ordered_length({0}3 v)\n{{\n    return sqrt(((v[0] * v[0] + v[1] * "
                                       "v[1]) "
                                       "+ v[2] * v[2]));\n}}\n",
                                       half)))
            .dump("text", text);
        // the constants are halves, so no step computes in floats
        CHECK(text.contains(cc::format("return t * (t * (({0})3.0 - ({0})2.0 * t));", half))).dump("text", text);
    }
}

TEST("sgl float - a text's cross is each lane's a_p * b_q - a_q * b_p at .precise, and the target's at .fast "
     "(EMIT-281)")
{
    constexpr cc::string_view source = "require shader_f16\n"
                                       "\n"
                                       "binding work:\n"
                                       "    values: mut buffer[float4]\n"
                                       "\n"
                                       "fun crossed(id: int3){work} -> float4:\n"
                                       "    let v = work.values[id.x]\n"
                                       "    let w = work.values[0]\n"
                                       "    let h = half3(v.x as half, v.y as half, v.z as half)\n"
                                       "    let g = half3(w.x as half, w.y as half, w.z as half)\n"
                                       "    let c = cross(h, g)\n"
                                       "    let f = cross(v.xyz, w.xyz)\n"
                                       "    let e = cross(vec3(v.x, v.y, v.z), vec3(w.x, w.y, w.z))\n"
                                       "    return float4(c.x as float, f.y, e.z, 0.0)\n"
                                       "\n"
                                       "@compute(64) fun loose(@thread_id id: int3){work}:\n"
                                       "    work.values[id.x] = crossed(id)\n"
                                       "\n"
                                       "@float(.precise) @compute(64) fun strict(@thread_id id: int3){work}:\n"
                                       "    work.values[id.x] = crossed(id)\n";
    auto const of = [&](cc::string_view entry, target t)
    {
        auto const e
            = sgl::compile_to_text({.source = source, .source_name = "cross.sgl", .entry_point = entry, .target = t});
        REQUIRE(e.has_value())
            .dump("target", sgl::emit::to_string(t))
            .dump("error", e.has_error() ? e.error() : cc::string());
        return e.value().text;
    };
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl, target::wgsl})
    {
        auto const loose = of("loose", t);
        CHECK(loose.contains("cross(h, g)")).dump("text", loose);
        CHECK(loose.contains("cross(v.xyz, w.xyz)")).dump("text", loose);
        CHECK(!loose.contains("sgl_ordered_")).dump("text", loose);
    }
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
    {
        auto const strict = of("strict", t);
        auto const half = t == target::msl ? cc::string_view("half3") : cc::string_view("float16_t3");
        for (auto const type : {half, cc::string_view("float3")})
            CHECK(strict.contains(cc::format("{0} sgl_ordered_cross({0} a, {0} b)\n{{\n    return {0}(a[1] * b[2] - "
                                             "a[2] * "
                                             "b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);\n}}\n",
                                             type)))
                .dump("type", type)
                .dump("text", strict);
        // a vec3's and a float3's helper are one text, written once
        CHECK(bare_calls(strict, "sgl_ordered_cross") == 5).dump("text", strict);
        CHECK(bare_calls(strict, "cross") == 0).dump("text", strict);
    }
}

TEST("sgl float - a named NaN or infinity is written through its bits on every target and route (EMIT-287)")
{
    constexpr cc::string_view source = "require float_specials, shader_f16, shader_f64\n"
                                       "\n"
                                       "binding work:\n"
                                       "    values: mut buffer[float]\n"
                                       "binding narrow:\n"
                                       "    halves: mut buffer[half2]\n"
                                       "binding wide:\n"
                                       "    doubles: mut buffer[double]\n"
                                       "\n"
                                       "@float(.specials) @compute(64) fun floats(@thread_id id: int3){work}:\n"
                                       "    work.values[id.x] = select(id.x > 3, float.nan, -float.infinity)\n"
                                       "\n"
                                       "@float(.specials) @compute(64) fun halves(@thread_id id: int3){narrow}:\n"
                                       "    narrow.halves[id.x] = half2(half.nan, 1.0)\n"
                                       "\n"
                                       "@float(.specials) @compute(64) fun doubles(@thread_id id: int3){wide}:\n"
                                       "    wide.doubles[id.x] = double.infinity\n";
    auto const of = [&](cc::string_view entry, target t, sgl::emit::native_route native)
    {
        auto const e = sgl::compile_to_text({.source = source,
                                             .source_name = "named.sgl",
                                             .entry_point = entry,
                                             .target = t,
                                             .weakens_float = t == target::wgsl,
                                             .native = native});
        REQUIRE(e.has_value())
            .dump("entry", entry)
            .dump("target", sgl::emit::to_string(t))
            .dump("error", e.has_error() ? e.error() : cc::string());
        return e.value();
    };
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const text = of("floats", t, sgl::emit::native_route::text).text;
        CHECK(text.contains("select(id.x > 3, asfloat(0x7fc00000u), -asfloat(0x7f800000u))")).dump("text", text);
        CHECK(of("halves", t, sgl::emit::native_route::text).text.contains("asfloat16(uint16_t(0x7e00u))"));
        CHECK(of("doubles", t, sgl::emit::native_route::text).text.contains("asdouble(0x00000000u, 0x7ff00000u)"));
    }
    auto const msl = of("floats", target::msl, sgl::emit::native_route::text).text;
    CHECK(msl.contains("as_type<float>(0x7fc00000u)")).dump("text", msl);
    CHECK(msl.contains("-as_type<float>(0x7f800000u)")).dump("text", msl);
    CHECK(of("halves", target::msl, sgl::emit::native_route::text).text.contains("as_type<half>(ushort(0x7e00))"));

    // a `let` is no const-expression, so WGSL creates the shader without folding the bits into a value it refuses
    auto const wgsl = of("floats", target::wgsl, sgl::emit::native_route::text).text;
    CHECK(wgsl.contains("fn sgl_non_finite_f32_7fc00000() -> f32 {\n    let bits = 0x7fc00000u;\n    return "
                        "bitcast<f32>(bits);\n}\n"))
        .dump("text", wgsl);
    CHECK(wgsl.contains("select(-sgl_non_finite_f32_7f800000(), sgl_non_finite_f32_7fc00000(), id.x > 3)")).dump("text", wgsl);
    auto const wgsl_half = of("halves", target::wgsl, sgl::emit::native_route::text).text;
    CHECK(wgsl_half.contains("    let bits = 0x7e00u;\n    return bitcast<vec2<f16>>(bits).x;\n")).dump("text", wgsl_half);

    // SGL's own DXIL and SPIR-V hold a constant of the bits
    for (auto const entry : {"floats", "halves", "doubles"})
    {
        CHECK(!of(entry, target::hlsl_dx12, sgl::emit::native_route::require).dxil.empty()).dump("entry", entry);
        auto const spirv = of(entry, target::hlsl_vulkan, sgl::emit::native_route::require).spirv;
        auto holds = false;
        for (auto at = isize(5); at < spirv.size(); at += isize(spirv[at] >> 16))
            if ((spirv[at] & 0xffffu) == 43) // OpConstant
                holds = holds || spirv[at + 3] == 0x7fc00000u || spirv[at + 3] == 0xff800000u || spirv[at + 3] == 0x7e00u
                     || (spirv[at + 3] == 0u && at + 4 < spirv.size() && (spirv[at] >> 16) == 5
                         && spirv[at + 4] == 0x7ff00000u);
        CHECK(holds).dump("entry", entry);
    }
}
