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
    // a placeholder naming nothing, and one reaching into a parameter
    CHECK(reports_of("fun f(x: float) -> float:\n"
                     "    \"hlsl\n"
                     "        $result = $nope;\n"
                     "    \"\n")
              .contains("$nope names no parameter of f"));
    CHECK(reports_of("fun f(p: float2) -> float:\n"
                     "    \"hlsl\n"
                     "        $result = $p.x;\n"
                     "    \"\n")
              .contains("names a parameter or `result`, and nothing of either"));
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
