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

cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view entry, target t, bool weakens = false)
{
    return sgl::compile_to_text(
        {.source = k_layered, .source_name = "float.sgl", .entry_point = entry, .target = t, .weakens_float = weakens});
}
} // namespace

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
