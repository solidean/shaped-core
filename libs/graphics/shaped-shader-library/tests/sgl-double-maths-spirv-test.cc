#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

using namespace cc::primitive_defines;

// EMIT-256: on vulkan a double's roundings and roots are GLSL.std.450's own, from both routes.
// DXC hands a double to its own `floor` or `sqrt` as a float, an OpFConvert either side, so this reads the words of
// each route's module and refuses any conversion, whatever spirv-val accepts.

namespace
{
constexpr u32 k_op_ext_inst = 12;
constexpr u32 k_op_type_float = 22;
constexpr u32 k_op_fconvert = 115;

constexpr cc::string_view k_source
    = "require shader_f64\n"
      "\n"
      "binding work:\n"
      "    values: mut buffer[double4]\n"
      "\n"
      "@compute(64) fun run(@thread_id id: int3){work}:\n"
      "    let v = work.values[2 * id.x]\n"
      "    work.values[2 * id.x] = double4(sqrt(v.x), inverse_sqrt(v.y), floor(v.z), ceil(v.w))\n"
      "    work.values[2 * id.x + 1] = double4(round(v.x), trunc(v.y), fract(v.z), 0.0f64)\n";

/// GLSL.std.450's Trunc, Floor, Ceil, RoundEven, Fract, Sqrt and InverseSqrt, which EMIT-256 names.
constexpr u32 k_expected[] = {2, 3, 8, 9, 10, 31, 32};

struct double_walk
{
    /// Each GLSL.std.450 instruction a 64-bit float result takes, by its number.
    cc::vector<u32> on_doubles;
    i32 conversions = 0;
    bool readable = true;
};

[[nodiscard]] double_walk walk(cc::span<u32 const> module)
{
    auto result = double_walk();
    REQUIRE(module.size() >= 5);
    auto const bound = isize(module[3]);
    auto is_double = cc::vector<bool>::create_filled(bound, false);
    for (auto at = isize(5); at < module.size();)
    {
        auto const count = isize(module[at] >> 16);
        auto const op = module[at] & 0xffffu;
        if (count <= 0 || at + count > module.size())
        {
            result.readable = false;
            break;
        }
        auto const word = [&](isize i) { return i < count ? module[at + i] : 0xffffffffu; };
        if (op == k_op_type_float && word(2) == 64 && word(1) < u32(bound))
            is_double[word(1)] = true;
        if (op == k_op_fconvert)
            ++result.conversions;
        if (op == k_op_ext_inst && word(1) < u32(bound) && is_double[word(1)])
            result.on_doubles.push_back(word(4));
        at += count;
    }
    return result;
}

[[nodiscard]] cc::vector<u32> words_of(sg::compiled_shader const& shader)
{
    auto words = cc::vector<u32>();
    auto const bytes = cc::span<byte const>(shader.bytecode.data(), shader.bytecode.size());
    for (auto i = isize(0); i + 3 < bytes.size(); i += 4)
        words.push_back(u32(bytes[i]) | u32(bytes[i + 1]) << 8 | u32(bytes[i + 2]) << 16 | u32(bytes[i + 3]) << 24);
    return words;
}

void check_holds_each(double_walk const& w, char const* route)
{
    CHECK(w.readable).dump("route", route);
    CHECK(w.conversions == 0).dump("route", route);
    for (auto const expected : k_expected)
    {
        auto found = false;
        for (auto const n : w.on_doubles)
            found = found || n == expected;
        CHECK(found).dump("route", route).dump("GLSL.std.450", expected);
    }
}
} // namespace

TEST("slib - a vulkan double's roundings and roots are GLSL.std.450's on doubles, from SGL's binary (EMIT-256)")
{
    auto const e = sgl::compile_to_text({.source = k_source,
                                         .entry_point = "run",
                                         .target = sgl::emit::target::hlsl_vulkan,
                                         .native = sgl::emit::native_route::require});
    REQUIRE(e.has_value());
    REQUIRE(!e.value().spirv.empty());
    check_holds_each(walk(e.value().spirv), "sgl");
}

TEST("slib - a vulkan double's roundings and roots are GLSL.std.450's on doubles, through DXC (EMIT-256)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const e
        = sgl::compile_to_text({.source = k_source, .entry_point = "run", .target = sgl::emit::target::hlsl_vulkan});
    REQUIRE(e.has_value());
    auto const shader = compiler.value().compile(
        {.source = e.value().text, .entry_point = e.value().entry_point, .stage = sg::shader_stage::compute},
        {.target = ssc::dxc::compile_target::spirv, .warnings_as_errors = false});
    REQUIRE(shader.has_value());
    check_holds_each(walk(words_of(shader.value())), "dxc");
}
