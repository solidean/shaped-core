#include <shaped-shader-library/binding/impl/hlsl_binding_types.hh>

namespace
{
using sg::binding_type;
using sg::texture_view_dimension;

/// One row of the table.
/// `dimension` is read only for the two texture kinds, which is what the entries below leave at their default say.
struct table_entry
{
    cc::string_view name;
    char register_class = 't';
    binding_type type = binding_type::readonly_texture;
    texture_view_dimension dimension = texture_view_dimension::tex_2d;
};

constexpr table_entry k_table[] = {
    {"Texture1D", 't', binding_type::readonly_texture, texture_view_dimension::tex_1d},
    {"Texture1DArray", 't', binding_type::readonly_texture, texture_view_dimension::tex_1d_array},
    {"Texture2D", 't', binding_type::readonly_texture, texture_view_dimension::tex_2d},
    {"Texture2DArray", 't', binding_type::readonly_texture, texture_view_dimension::tex_2d_array},
    {"Texture2DMS", 't', binding_type::readonly_texture, texture_view_dimension::tex_2d_ms},
    {"Texture2DMSArray", 't', binding_type::readonly_texture, texture_view_dimension::tex_2d_ms_array},
    {"Texture3D", 't', binding_type::readonly_texture, texture_view_dimension::tex_3d},
    {"TextureCube", 't', binding_type::readonly_texture, texture_view_dimension::cube},
    {"TextureCubeArray", 't', binding_type::readonly_texture, texture_view_dimension::cube_array},

    // A storage view has no cube and no multisampling, which is why this half of the table is shorter.
    {"RWTexture1D", 'u', binding_type::readwrite_texture, texture_view_dimension::tex_1d},
    {"RWTexture1DArray", 'u', binding_type::readwrite_texture, texture_view_dimension::tex_1d_array},
    {"RWTexture2D", 'u', binding_type::readwrite_texture, texture_view_dimension::tex_2d},
    {"RWTexture2DArray", 'u', binding_type::readwrite_texture, texture_view_dimension::tex_2d_array},
    {"RWTexture3D", 'u', binding_type::readwrite_texture, texture_view_dimension::tex_3d},

    {"Buffer", 't', binding_type::readonly_structured_buffer},
    {"StructuredBuffer", 't', binding_type::readonly_structured_buffer},
    {"RWStructuredBuffer", 'u', binding_type::readwrite_structured_buffer},
    {"ByteAddressBuffer", 't', binding_type::readonly_raw_buffer},
    {"RWByteAddressBuffer", 'u', binding_type::readwrite_raw_buffer},
    {"ConstantBuffer", 'b', binding_type::uniform_buffer},

    {"SamplerState", 's', binding_type::sampler},
    {"SamplerComparisonState", 's', binding_type::sampler},

    {"RaytracingAccelerationStructure", 't', binding_type::acceleration_structure},
};

[[nodiscard]] bool is_texture(binding_type type)
{
    return type == binding_type::readonly_texture || type == binding_type::readwrite_texture;
}
} // namespace

cc::optional<slib::impl::hlsl_binding_type> slib::impl::binding_type_of(cc::string_view hlsl_type)
{
    for (auto const& entry : k_table)
    {
        if (entry.name != hlsl_type)
            continue;

        hlsl_binding_type result;
        result.register_class = entry.register_class;
        result.type = entry.type;
        if (is_texture(entry.type))
            result.dimension = entry.dimension;
        return result;
    }

    return cc::nullopt;
}
