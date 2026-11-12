#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/test_source.hh>
#include <shaped-graphics-language/emit/emit.hh>

using sgl::emit::target;

// CHK-427: a function's body may be a target's own text, a tagged string such as `"hlsl`, one per language.

namespace
{
constexpr cc::string_view k_rsqrt = "binding work:\n"
                                    "    values: mut buffer[float]\n"
                                    "\n"
                                    "fun fast_rsqrt(x: float) -> float:\n"
                                    "    const if target_language == .hlsl:\n"
                                    "        \"hlsl\n"
                                    "            $result = rsqrt($x);\n"
                                    "        \"\n"
                                    "    else if target_api == .metal:\n"
                                    "        \"msl\n"
                                    "            $result = metal::fast::rsqrt($x);\n"
                                    "        \"\n"
                                    "    else:\n"
                                    "        return 1.0 / sqrt(x)\n"
                                    "\n"
                                    "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                    "    work.values[id.x] = fast_rsqrt(work.values[id.x] + 1.0)\n";

constexpr cc::string_view k_atomic = "binding work:\n"
                                     "    sum: mut buffer[atomic[float]]\n"
                                     "\n"
                                     "fun add_native(a: mut atomic[float], v: float) -> float:\n"
                                     "    const if has(.float_atomic_add) and target_api == .metal:\n"
                                     "        \"msl\n"
                                     "            $result = atomic_fetch_add_explicit(&$a, $v, memory_order_relaxed);\n"
                                     "        \"\n"
                                     "    else:\n"
                                     "        return a.add(v)\n"
                                     "\n"
                                     "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                     "    add_native(mut work.sum[0], 1.0)\n";

cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view source, target t, sgl::check::feature_set device = {})
{
    return sgl::compile_to_text({.source = source, .entry_point = "main_cs", .target = t, .device_features = device});
}

/// The diagnostics a source reports when its tests run, which is where a body's rules are judged.
cc::string reports_of(cc::string_view source)
{
    auto const tested = sgl::test_source(source, "<target-text>");
    return tested.errors;
}
} // namespace

TEST("sgl target text - each target writes its own text, with each placeholder the argument it names")
{
    auto const hlsl = compiled(k_rsqrt, target::hlsl_dx12);
    auto const msl = compiled(k_rsqrt, target::msl);
    auto const wgsl = compiled(k_rsqrt, target::wgsl);
    REQUIRE(hlsl.has_value());
    REQUIRE(msl.has_value());
    REQUIRE(wgsl.has_value());
    CHECK(hlsl.value().text.contains("float fast_rsqrt_text;\n"));
    CHECK(hlsl.value().text.contains("fast_rsqrt_text = rsqrt(x);\n"));
    CHECK(msl.value().text.contains("fast_rsqrt_text = metal::fast::rsqrt(x);\n"));
    // a target without a body of its own takes the SGL one
    CHECK(wgsl.value().text.contains("1.0 / sqrt(x)"));
    CHECK(!wgsl.value().text.contains("fast_rsqrt_text"));
}

TEST("sgl target text - a parameter of an atomic hands the text the place the call named")
{
    auto const native = compiled(k_atomic, target::msl,
                                 []
                                 {
                                     auto f = sgl::check::feature_set();
                                     f.set(sgl::check::feature::float_atomic_add);
                                     return f;
                                 }());
    auto const loop = compiled(k_atomic, target::msl);
    REQUIRE(native.has_value());
    REQUIRE(loop.has_value());
    CHECK(native.value().text.contains("atomic_fetch_add_explicit(&work_sum[0], 1.0, memory_order_relaxed);"));
    CHECK(loop.value().text.contains("atomic_compare_exchange_weak_explicit"));
    // the footprint reads the call against its function's parameters, so the atomic's buffer is written
    CHECK(sgl::check::footprint_text(native.value().footprint) == "work.sum: read write");
}

TEST("sgl target text - a run of tagged strings is one body, one per language, of which a target writes its own")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "fun touch(i: int):\n"
                            "    \"hlsl\n"
                            "        work_values[$i] = 7.0f;\n"
                            "    \"\n"
                            "    \"msl\n"
                            "        work_values[$i] = 8.0f;\n"
                            "    \"\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    touch(id.x)\n";
    auto const hlsl = compiled(source, target::hlsl_vulkan);
    auto const msl = compiled(source, target::msl);
    auto const wgsl = compiled(source, target::wgsl);
    REQUIRE(hlsl.has_value());
    REQUIRE(msl.has_value());
    CHECK(hlsl.value().text.contains("work_values[id.x] = 7.0f;\n"));
    CHECK(msl.value().text.contains("work_values[id.x] = 8.0f;\n"));
    // a void body is a statement of its own, with nothing left behind it
    CHECK(!hlsl.value().text.contains(";;"));
    // a target with no body in its language is refused, never given another's
    REQUIRE(wgsl.has_error());
    CHECK(wgsl.error().contains("invalid-target-text"));
    CHECK(wgsl.error().contains("touch has no body in wgsl"));
}

TEST("sgl target text - a test runs no target's text, so it reaches only an SGL branch")
{
    constexpr auto fallback = "fun twice(x: float) -> float:\n"
                              "    const if target_language == .hlsl:\n"
                              "        \"hlsl\n"
                              "            $result = 2.0 * $x;\n"
                              "        \"\n"
                              "    else:\n"
                              "        return x + x\n"
                              "\n"
                              "test twice(1.5) == 3.0\n";
    CHECK(reports_of(fallback) == "");

    constexpr auto text_only = "fun twice(x: float) -> float:\n"
                               "    \"hlsl\n"
                               "        $result = 2.0 * $x;\n"
                               "    \"\n"
                               "\n"
                               "test twice(1.5) == 3.0\n";
    CHECK(reports_of(text_only).contains("invalid-target-text"));
    CHECK(reports_of(text_only).contains("a test reaches twice"));
}

TEST("sgl target text - a body's text stands where a body does, in a language a target writes, naming parameters")
{
    // beside SGL in one body
    CHECK(reports_of("fun f(x: float) -> float:\n"
                     "    let y = x\n"
                     "    \"hlsl\n"
                     "        $result = $x;\n"
                     "    \"\n")
              .contains("stands only as a function's whole body"));
    // a language no target writes
    CHECK(reports_of("fun f(x: float) -> float:\n"
                     "    \"glsl\n"
                     "        $result = $x;\n"
                     "    \"\n")
              .contains("glsl names no language a target writes"));
    // two bodies in one language
    CHECK(reports_of("fun f(x: float) -> float:\n"
                     "    \"hlsl\n"
                     "        $result = $x;\n"
                     "    \"\n"
                     "    \"hlsl\n"
                     "        $result = -$x;\n"
                     "    \"\n")
              .contains("f has a second body in hlsl"));
    // a placeholder naming nothing
    CHECK(reports_of("fun f(x: float) -> float:\n"
                     "    \"hlsl\n"
                     "        $result = $nope;\n"
                     "    \"\n")
              .contains("$nope names no parameter of f"));
    // `$p.y` is the target's member of the argument, which is written as a primary
    auto const member = sgl::compile_to_text({.source = "binding work:\n"
                                                        "    values: mut buffer[float]\n"
                                                        "\n"
                                                        "fun f(p: float2) -> float:\n"
                                                        "    \"hlsl\n"
                                                        "        $result = $p.y;\n"
                                                        "    \"\n"
                                                        "\n"
                                                        "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                                        "    work.values[id.x] = f(float2(1.0, work.values[0]))\n",
                                              .entry_point = "main_cs",
                                              .target = target::hlsl_dx12});
    REQUIRE(member.has_value());
    CHECK(member.value().text.contains(".y;"));
    CHECK(reports_of("fun f(p: float2) -> float:\n"
                     "    \"hlsl\n"
                     "        $result = $p.y;\n"
                     "    \"\n")
          == "");
    // `$result` is exactly where there is a result
    CHECK(reports_of("fun f(x: float):\n"
                     "    \"hlsl\n"
                     "        $result = $x;\n"
                     "    \"\n")
              .contains("f gives nothing, so its text has no `$result`"));
    CHECK(reports_of("fun f(x: float) -> float:\n"
                     "    \"hlsl\n"
                     "        float y = $x;\n"
                     "    \"\n")
              .contains("f gives a float, which its hlsl text assigns to `$result`"));
    CHECK(reports_of("fun f(result: float) -> float:\n"
                     "    \"hlsl\n"
                     "        $result = 1.0;\n"
                     "    \"\n")
              .contains("no parameter of it is named result"));
}

TEST("sgl target text - a file's preamble is written once ahead of an entry point that writes a body of that file")
{
    constexpr auto source = "\"hlsl\n"
                            "    float doubled_spirv(float x);\n"
                            "\"\n"
                            "\n"
                            "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "fun twice(x: float) -> float:\n"
                            "    const if target_api == .vulkan:\n"
                            "        \"hlsl\n"
                            "            $result = $x * 2.0;\n"
                            "        \"\n"
                            "    else:\n"
                            "        return x + x\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = twice(twice(work.values[id.x]))\n";
    auto const vulkan = compiled(source, target::hlsl_vulkan);
    auto const dx12 = compiled(source, target::hlsl_dx12);
    REQUIRE(vulkan.has_value());
    REQUIRE(dx12.has_value());
    auto const& text = vulkan.value().text;
    auto const at = text.find("float doubled_spirv(float x);\n");
    REQUIRE(at >= 0);
    // once, whatever number of calls reach the file's bodies, and ahead of the entry point
    CHECK(!cc::string_view(text).subview(at + 1).contains("float doubled_spirv"));
    CHECK(at < text.find("void main_cs"));
    // a compile that writes none of the file's text writes none of its preamble
    CHECK(!dx12.value().text.contains("doubled_spirv"));
}

TEST("sgl target text - a preamble is in a language a target writes, and has no placeholder")
{
    CHECK(reports_of("\"glsl\n"
                     "    float f();\n"
                     "\"\n")
              .contains("glsl names no language a target writes"));
    CHECK(reports_of("\"hlsl\n"
                     "    float f($x);\n"
                     "\"\n")
              .contains("a preamble belongs to no call, so it has no placeholder"));
}

namespace
{
/// A source whose entry point runs `before`, branches on `condition`, then calls `call` under the branch.
cc::string branching_on(cc::string_view functions,
                        cc::string_view condition,
                        cc::string_view call,
                        cc::string_view before = "let lane = 0")
{
    return cc::format("binding work:\n"
                      "    values: mut buffer[float]\n"
                      "\n"
                      "{}\n"
                      "@compute(64) fun main_cs(@thread_id id: int3){{work}}:\n"
                      "    {}\n"
                      "    if {}:\n"
                      "        work.values[lane] = {}\n",
                      functions, before, condition, call);
}

constexpr cc::string_view k_lane_functions = "@subgroup @pure fun lane_sum(x: float) -> float:\n"
                                             "    \"hlsl\n"
                                             "        $result = WaveActiveSum($x);\n"
                                             "    \"\n"
                                             "\n"
                                             "fun lane_index() -> int:\n"
                                             "    \"hlsl\n"
                                             "        $result = WaveGetLaneIndex();\n"
                                             "    \"\n"
                                             "\n"
                                             "@pure fun doubled(x: int) -> int:\n"
                                             "    \"hlsl\n"
                                             "        $result = 2 * $x;\n"
                                             "    \"\n"
                                             "\n"
                                             "fun read_lane(i: mut int):\n"
                                             "    \"hlsl\n"
                                             "        $i = WaveGetLaneIndex();\n"
                                             "    \"\n";
} // namespace

TEST("sgl target text - a body's declaration says what its text asks of the control flow, and what it gives")
{
    auto const uniform = compiled(branching_on(k_lane_functions, "doubled(3) > 4", "lane_sum(1.0)"), target::hlsl_dx12);
    CHECK(uniform.has_value());
    // CHK-431: `@subgroup` needs every invocation of the subgroup to reach the call
    auto const varying = compiled(branching_on(k_lane_functions, "id.x > 4", "lane_sum(1.0)"), target::hlsl_dx12);
    REQUIRE(varying.has_error());
    CHECK(varying.error().contains("non-uniform-control-flow"));
    CHECK(varying.error().contains("lane_sum exchanges values within the subgroup"));
    // without `@pure`, what a text gives is unproven, and so is what it writes through a `mut` parameter
    auto const unproven
        = compiled(branching_on(k_lane_functions, "lane_index() > 4", "lane_sum(1.0)"), target::hlsl_dx12);
    REQUIRE(unproven.has_error());
    CHECK(unproven.error().contains("lane_index gave, whose body is a target's text"));
    auto const written = compiled(
        branching_on(k_lane_functions, "lane > 4", "lane_sum(1.0)", "let mut lane = 0\n    read_lane(mut lane)"),
        target::hlsl_dx12);
    CHECK(written.has_error());
    // `@pure` makes it follow its arguments, and a value of no effect, so it runs no matter where it was asked for
    auto const pure = compiled(branching_on(k_lane_functions, "doubled(id.x) > 4", "lane_sum(1.0)"), target::hlsl_dx12);
    REQUIRE(pure.has_error());
    CHECK(pure.error().contains("comes from the stage input id"));
}

TEST("sgl target text - @barrier, @subgroup and @derivatives declare what a body in a target's text does")
{
    // a body in SGL says what it does itself
    CHECK(reports_of("@subgroup fun f(x: float) -> float => x\n\ntest f(1.0) == 1.0\n")
              .contains("@subgroup on a function whose body is SGL"));
    // a barrier is an effect
    CHECK(reports_of("@barrier @pure fun f():\n    \"hlsl\n        GroupMemoryBarrierWithGroupSync();\n    \"\n")
              .contains("a @barrier function is never @pure"));
    // `@barrier` needs the whole workgroup to reach it
    auto const fence = compiled(branching_on("@barrier fun fence():\n"
                                             "    \"hlsl\n"
                                             "        GroupMemoryBarrierWithGroupSync();\n"
                                             "    \"\n"
                                             "\n"
                                             "fun one() -> float:\n"
                                             "    fence()\n"
                                             "    return 1.0\n",
                                             "id.x > 4", "one()"),
                                target::hlsl_dx12);
    REQUIRE(fence.has_error());
    CHECK(fence.error().contains("fence waits for every thread of the workgroup"));
}

TEST("sgl target text - a body's text under has(.f) uses f, which is what the compile asks of the device")
{
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "fun scaled(x: float) -> float:\n"
                            "    const if has(.subgroups):\n"
                            "        \"hlsl\n"
                            "            $result = WaveActiveSum($x);\n"
                            "        \"\n"
                            "    else:\n"
                            "        return x\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = scaled(work.values[id.x])\n";
    auto device = sgl::check::feature_set();
    device.set(sgl::check::feature::subgroups);
    auto const with = compiled(source, target::hlsl_dx12, device);
    auto const without = compiled(source, target::hlsl_dx12);
    REQUIRE(with.has_value());
    REQUIRE(without.has_value());
    CHECK(with.value().features.has(sgl::check::feature::subgroups));
    CHECK(!without.value().features.has(sgl::check::feature::subgroups));
}

TEST("sgl target text - maybe_reorder asks shader execution reordering of the device that has it")
{
    constexpr auto source = "require raytracing_pipeline\n"
                            "\n"
                            "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@raygen fun main_cs(@launch_id id: int3){work}:\n"
                            "    maybe_reorder(id.x as uint, 1)\n"
                            "    work.values[id.x] = 1.0\n";
    auto device = sgl::check::feature_set();
    device.set(sgl::check::feature::shader_execution_reordering);
    auto const with = compiled(source, target::hlsl_dx12, device);
    REQUIRE(with.has_value());
    CHECK(with.value().text.contains("dx::MaybeReorderThread("));
    CHECK(with.value().features.has(sgl::check::feature::shader_execution_reordering));
    auto const without = compiled(source, target::hlsl_dx12);
    REQUIRE(without.has_value());
    CHECK(!without.value().text.contains("MaybeReorderThread"));
    CHECK(!without.value().features.has(sgl::check::feature::shader_execution_reordering));
}

TEST("sgl target text - a reordering trace takes its hint's bits from a named const")
{
    // CHK-433: the bits are checked as an expression like any other, so a name in them is flattened like one
    constexpr auto source
        = "require raytracing_pipeline\n"
          "\n"
          "const hint_bits = 2\n"
          "\n"
          "struct radiance:\n"
          "    color: float\n"
          "\n"
          "rays rs:\n"
          "    primary: radiance\n"
          "\n"
          "binding work:\n"
          "    world: acceleration_structure[.triangles]\n"
          "    values: mut buffer[float]\n"
          "\n"
          "@raygen fun main_cs(@launch_id id: int3){work}:\n"
          "    let mut p = radiance(0.0)\n"
          "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
          "    trace(work.world, r, rs.primary, mut p, reorder = id.x as uint, reorder_bits = hint_bits)\n"
          "    work.values[id.x] = p.color\n";
    auto device = sgl::check::feature_set();
    device.set(sgl::check::feature::shader_execution_reordering);
    auto const with = compiled(source, target::hlsl_dx12, device);
    REQUIRE(with.has_value()).dump("error", with.has_error() ? cc::string(with.error()) : cc::string());
    CHECK(with.value().text.contains("dx::MaybeReorderThread("));
}

TEST("sgl target text - a dx12 ray query sees the opacity micromaps of a device that has them")
{
    // EMIT-184: no program names the feature; the compile states it for each query
    constexpr auto source = "require ray_query\n"
                            "\n"
                            "binding work:\n"
                            "    world: acceleration_structure[.triangles]\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                            "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore).t\n";
    auto device = sgl::check::feature_set();
    device.set(sgl::check::feature::opacity_micromap);
    auto const with = compiled(source, target::hlsl_dx12, device);
    REQUIRE(with.has_value());
    CHECK(with.value().text.contains(", RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>"));
    CHECK(with.value().features.has(sgl::check::feature::opacity_micromap));
    // forcing 2 states is Shader Model 6.9's name, so only a compile that sees micromaps writes it into the template
    auto const forced
        = cc::string_view("require ray_query\n"
                          "\n"
                          "binding work:\n"
                          "    world: acceleration_structure[.triangles]\n"
                          "    values: mut buffer[float]\n"
                          "\n"
                          "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                          "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                          "    let flags = ray_flags.force_omm_2_state\n"
                          "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore, flags = flags).t\n");
    auto const forced_with = compiled(forced, target::hlsl_dx12, device);
    REQUIRE(forced_with.has_value());
    CHECK(forced_with.value().text.contains("RAY_FLAG_FORCE_OMM_2_STATE"));
    auto const forced_without = compiled(forced, target::hlsl_dx12);
    REQUIRE(forced_without.has_value());
    CHECK(!forced_without.value().text.contains("FORCE_OMM_2_STATE"));
    // a device without them is asked for nothing
    auto const without = compiled(source, target::hlsl_dx12);
    REQUIRE(without.has_value());
    CHECK(!without.value().text.contains("OPACITY_MICROMAPS"));
    CHECK(!without.value().features.has(sgl::check::feature::opacity_micromap));
    // vulkan's HLSL goes through DXC too, which wants the flag before it takes a forced ray, and the SPIR-V capability
    auto const vulkan = compiled(forced, target::hlsl_vulkan, device);
    REQUIRE(vulkan.has_value());
    CHECK(vulkan.value().text.contains(", RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>"));
    CHECK(vulkan.value().text.contains("[[vk::ext_capability(5381)]]"));
    CHECK(vulkan.value().features.has(sgl::check::feature::opacity_micromap));
    // vulkan sees micromaps without the flag, and DXC takes minutes over either and can write invalid SPIR-V, so a
    // query not forced to 2 states carries neither
    auto const vulkan_plain = compiled(source, target::hlsl_vulkan, device);
    REQUIRE(vulkan_plain.has_value());
    CHECK(!vulkan_plain.value().text.contains("OPACITY_MICROMAPS"));
    CHECK(!vulkan_plain.value().text.contains("ext_capability(5381)"));
}

TEST("sgl target text - a ray query takes the flags its trace hands it through a local as its template")
{
    // EMIT-159: a local given a constant and never assigned again is that constant; a mixed world's trace hands its
    // flags on through one
    constexpr auto source = "require ray_query\n"
                            "\n"
                            "binding work:\n"
                            "    world: acceleration_structure[.mixed]\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "struct ball:\n"
                            "    normal: vec3\n"
                            "\n"
                            "fun nothing(b: procedural_box) -> report[ball] => report.none()\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                            "    work.values[id.x] = work.world.trace(r, nothing, flags = "
                            "ray_flags.accept_first_hit_and_end_search).t\n";
    auto const out = compiled(source, target::hlsl_dx12);
    REQUIRE(out.has_value());
    CHECK(out.value().text.contains("RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>"));
    // EMIT-184: known flags that force nothing leave a vulkan query without the micromap flag and its capability
    auto device = sgl::check::feature_set();
    device.set(sgl::check::feature::opacity_micromap);
    auto const vulkan = compiled(source, target::hlsl_vulkan, device);
    REQUIRE(vulkan.has_value());
    CHECK(!vulkan.value().text.contains("OPACITY_MICROMAPS"));
    CHECK(!vulkan.value().text.contains("ext_capability(5381)"));
}
