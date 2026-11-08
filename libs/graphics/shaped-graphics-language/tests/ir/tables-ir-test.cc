#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace cc::primitive_defines;

// state_constant_tables, seen through the SPIR-V SGL writes: a table is a Private variable, a local a Function one.
// The DXIL writer states the same tables as internal constants, from the same program.

namespace
{
constexpr u32 k_op_variable = 59;
constexpr u32 k_private = 6;
constexpr u32 k_function = 7;

[[nodiscard]] cc::vector<u32> spirv_of(cc::string_view source)
{
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const e = sgl::compile_to_text(
            {.source = source, .entry_point = "cs", .target = t, .native = sgl::emit::native_route::require});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        if (t == sgl::emit::target::hlsl_vulkan)
            return e.value().spirv;
    }
    return {};
}

/// How many variables of `storage` the module declares.
[[nodiscard]] isize variables_of(cc::span<u32 const> words, u32 storage)
{
    auto count = isize(0);
    for (auto at = isize(5); at < words.size();)
    {
        auto const length = isize(words[at] >> 16);
        if ((words[at] & 0xffffu) == k_op_variable && length > 3 && words[at + 3] == storage)
            ++count;
        at += length > 0 ? length : 1;
    }
    return count;
}

[[nodiscard]] cc::string with_body(cc::string_view body)
{
    return cc::string("binding io:\n"
                      "    target: mut buffer[float]\n"
                      "\n"
                      "fun weight(i: int) -> float:\n"
                      "    let weights = [0.25, 0.5, 0.125, 2.0]\n"
                      "    return weights[i % 4]\n"
                      "\n"
                      "@compute(64) fun cs(@thread_id id: int3){io}:\n")
         + cc::string(body);
}
} // namespace

TEST("sgl ir tables - a let of constants an index reaches at run time is one table, however often it is inlined")
{
    auto const words = spirv_of(with_body("    io.target[id.x] = weight(id.x) + weight(id.x + 1)\n"));
    CHECK(variables_of(words, k_private) == 1);
    CHECK(variables_of(words, k_function) == 0);
}

TEST("sgl ir tables - a let read at constant indices alone stays values, and a written array stays memory")
{
    auto const words = spirv_of(with_body("    let fixed = [1.0, 2.0, 3.0]\n"
                                          "    let mut written = [1.0, 2.0, 3.0]\n"
                                          "    written[id.x % 3] = 4.0\n"
                                          "    io.target[id.x] = fixed[1] + written[id.y % 3]\n"));
    CHECK(variables_of(words, k_private) == 0);
    CHECK(variables_of(words, k_function) == 1);
}

TEST("sgl ir tables - a let whose value an invocation computes is no table")
{
    auto const words = spirv_of(with_body("    let read = [float(id.x), 2.0]\n"
                                          "    io.target[id.x] = read[id.y % 2] + weight(id.y)\n"));
    CHECK(variables_of(words, k_private) == 1);
    CHECK(variables_of(words, k_function) == 1);
}
