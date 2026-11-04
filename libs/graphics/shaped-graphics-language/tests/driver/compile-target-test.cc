#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/emit/emit.hh>

using sgl::emit::target;

// CHK-426: what a compile writes for is a constant once a body is inlined, which a `const if` branches on.

namespace
{
constexpr cc::string_view k_by_api = "binding work:\n"
                                     "    values: mut buffer[int]\n"
                                     "\n"
                                     "fun api_code() -> int:\n"
                                     "    const if target_api == .vulkan:\n"
                                     "        return 11\n"
                                     "    else if target_api == .dx12:\n"
                                     "        return 12\n"
                                     "    else if target_language == .msl:\n"
                                     "        return 13\n"
                                     "    return 14\n"
                                     "\n"
                                     "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                     "    work.values[id.x] = api_code()\n";

// subgroup_add needs `subgroups`, which only the branch `has` takes may use without a `require`
constexpr cc::string_view k_by_feature = "binding work:\n"
                                         "    values: mut buffer[float]\n"
                                         "\n"
                                         "fun total(v: float) -> float:\n"
                                         "    const if has(.subgroups):\n"
                                         "        return subgroup_add(v)\n"
                                         "    return v\n"
                                         "\n"
                                         "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                         "    work.values[id.x] = total(work.values[id.x])\n";

cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view source, target t, sgl::check::feature_set device = {})
{
    return sgl::compile_to_text({.source = source, .entry_point = "main_cs", .target = t, .device_features = device});
}

sgl::check::feature_set with(sgl::check::feature f)
{
    auto result = sgl::check::feature_set();
    result.set(f);
    return result;
}
} // namespace

TEST("sgl compile target - each target takes the branch of its own API and language")
{
    auto const vulkan = compiled(k_by_api, target::hlsl_vulkan);
    auto const dx12 = compiled(k_by_api, target::hlsl_dx12);
    auto const metal = compiled(k_by_api, target::msl);
    auto const webgpu = compiled(k_by_api, target::wgsl);
    REQUIRE(vulkan.has_value());
    REQUIRE(dx12.has_value());
    REQUIRE(metal.has_value());
    REQUIRE(webgpu.has_value());
    CHECK(vulkan.value().text.contains("= 11;"));
    CHECK(dx12.value().text.contains("= 12;"));
    CHECK(metal.value().text.contains("= 13;"));
    CHECK(webgpu.value().text.contains("= 14;"));
    // the branches a target does not take are in none of its text
    CHECK(!vulkan.value().text.contains("= 12;"));
    CHECK(!webgpu.value().text.contains("= 11;"));
}

TEST("sgl compile target - has answers what the device has, and grants the branch it takes its feature")
{
    auto const without = compiled(k_by_feature, target::hlsl_vulkan);
    auto const with_subgroups = compiled(k_by_feature, target::hlsl_vulkan, with(sgl::check::feature::subgroups));
    REQUIRE(without.has_value());
    REQUIRE(with_subgroups.has_value());
    CHECK(!without.value().text.contains("WaveActiveSum"));
    CHECK(with_subgroups.value().text.contains("WaveActiveSum"));
    // the device needs what the text it runs uses, and nothing where the branch is not taken
    CHECK(with_subgroups.value().features.has(sgl::check::feature::subgroups));
    CHECK(!without.value().features.has(sgl::check::feature::subgroups));
}

TEST("sgl compile target - a use outside the branch has granted still needs its require")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    if has(.subgroups) and id.x > 2:\n"
                            "        work.values[id.x] = 1.0\n"
                            "    work.values[id.x] = subgroup_add(work.values[id.x])\n";
    auto const text = compiled(source, target::hlsl_vulkan, with(sgl::check::feature::subgroups));
    REQUIRE(text.has_error());
    CHECK(text.error().contains("feature-not-declared"));
}

TEST("sgl compile target - a const holds no target, since a const is one value for every compile")
{
    constexpr auto computed = "const is_vulkan = target_api == .vulkan\n"
                              "binding work:\n"
                              "    values: mut buffer[int]\n"
                              "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                              "    work.values[id.x] = 1\n";
    constexpr auto named = "const api = target_api\n"
                           "binding work:\n"
                           "    values: mut buffer[int]\n"
                           "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                           "    work.values[id.x] = 1\n";
    constexpr auto asked = "const fast = has(.subgroups)\n"
                           "binding work:\n"
                           "    values: mut buffer[int]\n"
                           "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                           "    work.values[id.x] = 1\n";
    for (auto const source : {computed, named, asked})
    {
        auto const text = compiled(source, target::hlsl_vulkan);
        REQUIRE(text.has_error());
        CHECK(text.error().contains("invalid-constant-argument"));
    }
}

TEST("sgl compile target - a function returning the target is inlined where it is called, so it reads that compile's")
{
    constexpr auto source = "fun is_vulkan() => target_api == .vulkan\n"
                            "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    const if is_vulkan():\n"
                            "        work.values[id.x] = 21\n"
                            "    else:\n"
                            "        work.values[id.x] = 22\n";
    auto const vulkan = compiled(source, target::hlsl_vulkan);
    auto const dx12 = compiled(source, target::hlsl_dx12);
    REQUIRE(vulkan.has_value());
    REQUIRE(dx12.has_value());
    CHECK(vulkan.value().text.contains("= 21;"));
    CHECK(dx12.value().text.contains("= 22;"));
}

TEST("sgl compile target - the native float add is the device's float_atomic_add")
{
    constexpr auto source = "binding work:\n"
                            "    sum: mut buffer[atomic[float]]\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    work.sum[0].add(1.0)\n";
    auto const loop = compiled(source, target::msl);
    auto const native = compiled(source, target::msl, with(sgl::check::feature::float_atomic_add));
    REQUIRE(loop.has_value());
    REQUIRE(native.has_value());
    CHECK(native.value().text.contains("atomic_fetch_add_explicit"));
    CHECK(!loop.value().text.contains("atomic_fetch_add_explicit(&"));
}
