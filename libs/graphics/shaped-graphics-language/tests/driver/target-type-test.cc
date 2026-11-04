#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/test_source.hh>
#include <shaped-graphics-language/emit/emit.hh>

using sgl::emit::target;

// CHK-432: a struct whose body is a `const if` chain is spelled per compile, as the target's own type or as its fields.

namespace
{
constexpr cc::string_view k_handle = "binding work:\n"
                                     "    values: mut buffer[float]\n"
                                     "\n"
                                     "struct handle:\n"
                                     "    const if target_api == .dx12:\n"
                                     "        \"hlsl\n"
                                     "            uint2\n"
                                     "        \"\n"
                                     "    else:\n"
                                     "        a: float\n"
                                     "        b: float\n"
                                     "\n"
                                     "fun make(x: float) -> handle:\n"
                                     "    const if target_api == .dx12:\n"
                                     "        \"hlsl\n"
                                     "            $result = uint2(asuint($x), 7u);\n"
                                     "        \"\n"
                                     "    else:\n"
                                     "        return handle(x, 7.0)\n"
                                     "\n"
                                     "fun first(h: handle) -> float:\n"
                                     "    const if target_api == .dx12:\n"
                                     "        \"hlsl\n"
                                     "            $result = asfloat($h.x);\n"
                                     "        \"\n"
                                     "    else:\n"
                                     "        return h.a\n"
                                     "\n"
                                     "test first(make(2.0)) == 2.0\n"
                                     "\n"
                                     "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                     "    let h = make(work.values[id.x])\n"
                                     "    work.values[id.x] = first(h) * 2.0\n";

cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view source, target t)
{
    return sgl::compile_to_text({.source = source, .entry_point = "main_cs", .target = t});
}

cc::string reports_of(cc::string_view source)
{
    return sgl::test_source(source, "<target-type>").errors;
}

/// `k_handle` with `from` replaced by `to`, once.
cc::string handle_with(cc::string_view from, cc::string_view to)
{
    auto const text = k_handle;
    auto const at = cc::string(text).find(from);
    CC_ASSERT(at >= 0, "the replaced text must be in the source");
    auto result = cc::string(text.subview({.offset = 0, .size = at}));
    result += to;
    result += text.subview(at + from.size());
    return result;
}
} // namespace

TEST("sgl target type - a compile spells the type as its branch says, and a test runs the SGL one")
{
    auto const dx12 = compiled(k_handle, target::hlsl_dx12);
    auto const vulkan = compiled(k_handle, target::hlsl_vulkan);
    REQUIRE(dx12.has_value());
    REQUIRE(vulkan.has_value());
    // the target's own type, which nothing declares
    CHECK(dx12.value().text.contains("uint2 h;"));
    CHECK(dx12.value().text.contains("make_text = uint2(asuint(x), 7u);"));
    CHECK(!dx12.value().text.contains("struct handle"));
    // the fields, everywhere the SGL branch is taken
    CHECK(!vulkan.value().text.contains("uint2"));
    CHECK(compiled(k_handle, target::wgsl).has_value());
    CHECK(compiled(k_handle, target::msl).has_value());
    CHECK(reports_of(k_handle) == "");
}

TEST("sgl target type - a type spelled in a target's text has no field and no constructor there")
{
    // a field read outside the branch that has fields reaches a dx12 compile
    auto const read
        = compiled(handle_with("work.values[id.x] = first(h) * 2.0", "work.values[id.x] = h.a"), target::hlsl_dx12);
    REQUIRE(read.has_error());
    CHECK(read.error().contains("invalid-target-text"));
    CHECK(read.error().contains("its fields are its SGL branch's alone"));
    CHECK(compiled(handle_with("work.values[id.x] = first(h) * 2.0", "work.values[id.x] = h.a"), target::hlsl_vulkan)
              .has_value());
    // and so does a construction
    auto const built
        = compiled(handle_with("let h = make(work.values[id.x])", "let h = handle(1.0, 2.0)"), target::hlsl_dx12);
    REQUIRE(built.has_error());
    CHECK(built.error().contains("no SGL constructor builds"));
}

TEST("sgl target type - a compile whose branch has no spelling in its language is an error where the type is used")
{
    constexpr auto opaque = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "struct token:\n"
                            "    const if target_language == .hlsl:\n"
                            "        \"hlsl\n"
                            "            uint\n"
                            "        \"\n"
                            "\n"
                            "fun take() -> token:\n"
                            "    \"hlsl\n"
                            "        $result = 3u;\n"
                            "    \"\n"
                            "\n"
                            "fun spend(t: token) -> float:\n"
                            "    \"hlsl\n"
                            "        $result = float($t);\n"
                            "    \"\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = spend(take())\n";
    CHECK(compiled(opaque, target::hlsl_dx12).has_value());
    auto const wgsl = compiled(opaque, target::wgsl);
    REQUIRE(wgsl.has_error());
    CHECK(wgsl.error().contains("token has no branch this compile takes"));
}

TEST("sgl target type - a text type is held by locals, parameters and results alone, and spelled once per language")
{
    // CHK-397: no memory holds it, since a compile may spell it in its target's text
    CHECK(reports_of(handle_with("    values: mut buffer[float]\n", "    values: mut buffer[float]\n    hs: "
                                                                    "buffer[handle]\n"))
              .contains("unsupported-yet"));
    // one SGL branch, so the fields are one list for every target
    CHECK(reports_of("struct pair:\n"
                     "    const if target_api == .dx12:\n"
                     "        a: float\n"
                     "    else:\n"
                     "        b: float\n")
              .contains("pair has a second SGL branch"));
    // a condition reads the target and the device alone
    CHECK(reports_of("@option const fast = true\n"
                     "struct pair:\n"
                     "    const if fast:\n"
                     "        \"hlsl\n"
                     "            uint\n"
                     "        \"\n"
                     "    else:\n"
                     "        b: float\n")
              .contains("a text type's condition other than target_api"));
    // nothing stands beside the chain
    CHECK(reports_of("struct pair:\n"
                     "    const if target_api == .dx12:\n"
                     "        \"hlsl\n"
                     "            uint\n"
                     "        \"\n"
                     "    c: float\n")
              .contains("member-not-allowed-here"));
}
