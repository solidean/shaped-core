#include <clean-core/math/bit.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/ir/dxil/impl/llvm.hh>

using namespace cc::primitive_defines;

// What makes the native binaries as small as DXC's: constant tables, and `.fast`'s reassociation of constants.
// A table is seen through the SPIR-V SGL writes, a Private variable where a local is a Function one; DXIL states the
// same tables as internal constants, from the same program.

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

TEST("sgl dxil - at .fast a product of constants that cancel is the value it scaled, and is kept without the flags")
{
    namespace dxil = sgl::ir::dxil::impl;
    using dxil::binary_op;
    using dxil::fp_flags;
    auto const fast = u8(fp_flags::unsafe_algebra | fp_flags::no_nans | fp_flags::no_infs | fp_flags::no_signed_zeros
                         | fp_flags::allow_reciprocal);
    for (auto const flags : {fast, u8(0)})
    {
        auto m = dxil::module_builder();
        m.begin_function();
        m.open_block(m.add_block());
        auto const f32 = m.float_type();
        auto const memory = m.alloca_of(f32, 4);
        auto const x = m.load(f32, memory, 4);
        auto const c = m.constant_float(f32, cc::bit_cast<u32>(255.0f));
        auto const scaled = m.binary(binary_op::mul, x, c, flags);
        auto const before = m.instruction_count();
        auto const back = m.binary(binary_op::sdiv, scaled, c, flags);
        CHECK((back == x) == (flags == fast));
        CHECK((m.instruction_count() == before) == (flags == fast));
    }
}

namespace
{
/// The DXIL container SGL writes of `cs` storing `value` of the float4 `x`.
[[nodiscard]] cc::vector<byte> dxil_storing(cc::string_view value)
{
    auto const source = cc::string("binding io:\n"
                                   "    source: buffer[float4]\n"
                                   "    target: mut buffer[float4]\n"
                                   "\n"
                                   "@compute(64) fun cs(@thread_id id: int3){io}:\n"
                                   "    let x = io.source[id.x]\n"
                                   "    io.target[id.x] = ")
                      + cc::string(value) + "\n";
    auto const e = sgl::compile_to_text({.source = source,
                                         .entry_point = "cs",
                                         .target = sgl::emit::target::hlsl_dx12,
                                         .native = sgl::emit::native_route::require});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    return e.value().dxil;
}

[[nodiscard]] bool same_module(cc::string_view a, cc::string_view b)
{
    auto const x = dxil_storing(a);
    auto const y = dxil_storing(b);
    if (x.size() != y.size())
        return false;
    for (auto i = isize(0); i < x.size(); ++i)
        if (x[i] != y[i])
            return false;
    return true;
}
} // namespace

TEST("sgl dxil - at .fast a vector's product of constants that cancel writes the module of the vector alone")
{
    // simplify cancels it before the writer, so no constant of it is left in the module either
    CHECK(same_module("x * 255.0 / 255.0", "x"));
    CHECK(!same_module("x / 3.0", "x"));
}
