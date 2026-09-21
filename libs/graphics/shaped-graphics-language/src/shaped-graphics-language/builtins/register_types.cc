#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::value_kind;

/// A vector of floats with the fields `x y z [w]`, which every target spells as its own three- or four-vector.
type_record float_vector(cc::string_view name, i32 width, cc::string_view doc)
{
    auto declaration = cc::format("struct {}:", name);
    cc::string_view const fields[] = {"x", "y", "z", "w"};
    for (auto i = 0; i < width; ++i)
        declaration.appendf("\n    {}: float", fields[i]);

    auto const is_three = width == 3;
    return {
        .declaration = cc::move(declaration),
        .doc = doc,
        .hlsl = is_three ? "float3" : "float4",
        .wgsl = is_three ? "vec3f" : "vec4f",
        .msl = is_three ? "float3" : "float4",
        .hlsl_layout = {.size = width * 4, .alignment = 4},
        .wgsl_layout = {.size = width * 4, .alignment = 16},
        // MSL aligns a three-vector like WGSL and, unlike it, also sizes it 16: nothing fits into its tail.
        .msl_layout = {.size = 16, .alignment = 16},
        .leaf_kind = value_kind::scalar_float,
        .leaf_count = width,
        .crosses_edges = true,
    };
}
/// A vector of ints with the fields `x y z`, which is what a dispatch reports and what a buffer is indexed by.
type_record int_vector(cc::string_view name, i32 width, cc::string_view doc)
{
    auto declaration = cc::format("struct {}:", name);
    cc::string_view const fields[] = {"x", "y", "z", "w"};
    for (auto i = 0; i < width; ++i)
        declaration.appendf("\n    {}: int", fields[i]);

    auto const is_three = width == 3;
    return {
        .declaration = cc::move(declaration),
        .doc = doc,
        .hlsl = is_three ? "int3" : "int4",
        .wgsl = is_three ? "vec3i" : "vec4i",
        .msl = is_three ? "int3" : "int4",
        .hlsl_layout = {.size = width * 4, .alignment = 4},
        .wgsl_layout = {.size = width * 4, .alignment = 16},
        // MSL aligns and sizes a three-vector at 16, as it does a float3.
        .msl_layout = {.size = 16, .alignment = 16},
        .leaf_kind = value_kind::scalar_int,
        .leaf_count = width,
    };
}
} // namespace

void sgl::builtins::register_types(registry& r)
{
    r.add_comment("// A struct line without a block is opaque: there is no member to name.");
    r.add(type_record{
        .declaration = "struct float",
        .hlsl = "float",
        .wgsl = "f32",
        .msl = "float",
        .hlsl_layout = {.size = 4, .alignment = 4},
        .wgsl_layout = {.size = 4, .alignment = 4},
        .msl_layout = {.size = 4, .alignment = 4},
        .leaf_kind = value_kind::scalar_float,
        .leaf_count = 1,
        .crosses_edges = true,
    });

    r.add(float_vector("float3", 3, ""));
    r.add(float_vector("float4", 4, ""));
    r.add(float_vector("vec3", 3, "/// A direction: it has a length, and a translation leaves it alone."));
    r.add(float_vector("pos3", 3, "/// A position: a translation moves it."));
    r.add(float_vector("hpos4", 4, "/// A position in clip space, before the divide."));

    r.add(type_record{
        .declaration = "struct mat4",
        .doc = "/// Column-major, and a vector stands to its right.",
        .hlsl = "float4x4",
        .wgsl = "mat4x4f",
        .msl = "float4x4",
        // HLSL starts a matrix on a fresh 16-byte row, which is what an alignment of 16 says.
        .hlsl_layout = {.size = 64, .alignment = 16},
        .wgsl_layout = {.size = 64, .alignment = 16},
        .msl_layout = {.size = 64, .alignment = 16},
        .leaf_kind = value_kind::scalar_float,
        .leaf_count = 16,
    });

    r.add(type_record{
        .declaration = "struct int",
        .doc = "/// 32 bits, signed; its arithmetic wraps.",
        .hlsl = "int",
        .wgsl = "i32",
        .msl = "int",
        .hlsl_layout = {.size = 4, .alignment = 4},
        .wgsl_layout = {.size = 4, .alignment = 4},
        .msl_layout = {.size = 4, .alignment = 4},
        .leaf_kind = value_kind::scalar_int,
        .leaf_count = 1,
    });

    r.add(int_vector("int3", 3, "/// Three ints; what a compute dispatch reports as the thread's own id."));

    // No layout: a bool has a different size in every target's block, so it has no place in one.
    r.add(type_record{
        .declaration = "struct bool",
        .hlsl = "bool",
        .wgsl = "bool",
        .msl = "bool",
        .leaf_kind = value_kind::boolean,
        .leaf_count = 1,
    });
}
