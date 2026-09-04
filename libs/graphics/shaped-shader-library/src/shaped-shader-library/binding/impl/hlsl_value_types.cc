#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-shader-library/binding/impl/hlsl_value_types.hh>

namespace
{
using sg::vertex_attribute_format;

struct table_entry
{
    cc::string_view name;
    slib::impl::hlsl_value_type value;
};

// The mirror spells its members as plain `float` / `int` / `unsigned` and friends rather than reaching for a
// vector type, because generated package code sits below anything that could define one.
constexpr table_entry k_table[] = {
    {"float", {"float", 4, vertex_attribute_format::f32}},
    {"float2", {"float[2]", 8, vertex_attribute_format::vec2f}},
    {"float3", {"float[3]", 12, vertex_attribute_format::vec3f}},
    {"float4", {"float[4]", 16, vertex_attribute_format::vec4f}},

    {"int", {"int", 4, vertex_attribute_format::i32}},
    {"int2", {"int[2]", 8, vertex_attribute_format::vec2i}},
    {"int3", {"int[3]", 12, vertex_attribute_format::vec3i}},
    {"int4", {"int[4]", 16, vertex_attribute_format::vec4i}},

    {"uint", {"unsigned", 4, vertex_attribute_format::u32}},
    {"uint2", {"unsigned[2]", 8, vertex_attribute_format::vec2u}},
    {"uint3", {"unsigned[3]", 12, vertex_attribute_format::vec3u}},
    {"uint4", {"unsigned[4]", 16, vertex_attribute_format::vec4u}},
};
} // namespace

cc::optional<slib::impl::hlsl_value_type> slib::impl::value_type_of(cc::string_view hlsl_type)
{
    for (auto const& entry : k_table)
        if (entry.name == hlsl_type)
            return entry.value;

    return cc::nullopt;
}
