#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-shader-library/binding/impl/hlsl_value_types.hh>

using namespace cc::primitive_defines;

namespace
{
using sg::vertex_attribute_format;

/// One row of the table.
/// `format` is read only when `is_vertex_attribute`, which keeps the table constexpr -- a cc::optional in it
/// would not be.
struct table_entry
{
    cc::string_view name;
    cc::string_view cpp_type;
    isize size = 4;
    bool is_vertex_attribute = true;
    vertex_attribute_format format = vertex_attribute_format::f32;
};

// The mirror spells its members as plain `float` / `int` / `unsigned` and friends rather than reaching for a
// vector type, because generated package code sits below anything that could define one.
constexpr table_entry k_table[] = {
    {"float", "float", 4, true, vertex_attribute_format::f32},
    {"float2", "float[2]", 8, true, vertex_attribute_format::vec2f},
    {"float3", "float[3]", 12, true, vertex_attribute_format::vec3f},
    {"float4", "float[4]", 16, true, vertex_attribute_format::vec4f},

    {"int", "int", 4, true, vertex_attribute_format::i32},
    {"int2", "int[2]", 8, true, vertex_attribute_format::vec2i},
    {"int3", "int[3]", 12, true, vertex_attribute_format::vec3i},
    {"int4", "int[4]", 16, true, vertex_attribute_format::vec4i},

    {"uint", "unsigned", 4, true, vertex_attribute_format::u32},
    {"uint2", "unsigned[2]", 8, true, vertex_attribute_format::vec2u},
    {"uint3", "unsigned[3]", 12, true, vertex_attribute_format::vec3u},
    {"uint4", "unsigned[4]", 16, true, vertex_attribute_format::vec4u},

    // Four bytes in a constant block, and no vertex attribute format at all -- the reason sr::gpu_boolean exists.
    {"bool", "unsigned", 4, false},
};
} // namespace

cc::optional<slib::impl::hlsl_value_type> slib::impl::value_type_of(cc::string_view hlsl_type)
{
    for (auto const& entry : k_table)
    {
        if (entry.name != hlsl_type)
            continue;

        hlsl_value_type result;
        result.cpp_type = entry.cpp_type;
        result.size = entry.size;
        if (entry.is_vertex_attribute)
            result.format = entry.format;
        return result;
    }

    return cc::nullopt;
}
