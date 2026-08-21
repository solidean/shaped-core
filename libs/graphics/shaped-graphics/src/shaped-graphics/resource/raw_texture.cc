#include <clean-core/common/assert.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg
{
raw_texture::~raw_texture() = default;

raw_texture::raw_texture(texture_description const& desc) : _desc(desc)
{
    _desc.assert_valid();
}

bool texture_description::is_valid() const
{
    if (format == pixel_format::undefined)
        return false;
    if (width < 1 || height < 1 || depth < 1)
        return false;
    if (mip_levels < 1)
        return false;
    if (sample_count < 1)
        return false;
    if (array_layers.has_value() && array_layers.value() < 1)
        return false;

    // Shape invariants: array-ness / cube-ness / multisampling only combine with the dimensions that support them across every backend.
    if (dimension == texture_dimension::d1 && (is_cube || sample_count != 1))
        return false;
    if (dimension == texture_dimension::d3 && (is_cube || array_layers.has_value() || sample_count != 1))
        return false;
    if (is_cube && dimension != texture_dimension::d2)
        return false;
    if (sample_count > 1 && (dimension != texture_dimension::d2 || mip_levels != 1))
        return false;

    // allow_region_stream is D3D12's ALLOW_SIMULTANEOUS_ACCESS, which the runtime rejects on both of these.
    // Rejected here rather than per backend so the strictest backend's rule is the one every dev box sees.
    if (usage.has(texture_usage::allow_region_stream) && (usage.has(texture_usage::depth_stencil) || sample_count > 1))
        return false;

    return true;
}

void texture_description::assert_valid() const
{
    CC_ASSERT(format != pixel_format::undefined, "texture needs a concrete pixel_format");
    CC_ASSERT(width >= 1 && height >= 1 && depth >= 1, "texture extents must be >= 1");
    CC_ASSERT(mip_levels >= 1, "texture needs at least one mip level");
    CC_ASSERT(sample_count >= 1, "sample_count must be >= 1 (1 = not multisampled)");
    CC_ASSERT(!array_layers.has_value() || array_layers.value() >= 1, "array_layers, if set, must be >= 1");

    // Shape invariants: array-ness / cube-ness / multisampling only combine with the dimensions that support them across every backend.
    if (dimension == texture_dimension::d1)
        CC_ASSERT(!is_cube && sample_count == 1, "1D textures are neither cube nor multisampled");
    if (dimension == texture_dimension::d3)
        CC_ASSERT(!is_cube && !array_layers.has_value() && sample_count == 1, "3D textures are neither arrayed, cube, "
                                                                              "nor multisampled");
    if (is_cube)
        CC_ASSERT(dimension == texture_dimension::d2, "cube textures are 2D-faced");
    if (sample_count > 1)
    {
        CC_ASSERT(dimension == texture_dimension::d2, "multisampling is 2D only");
        CC_ASSERT(mip_levels == 1, "multisampled textures have a single mip level");
    }

    // allow_region_stream becomes D3D12's ALLOW_SIMULTANEOUS_ACCESS, which the runtime rejects on a depth/stencil or
    // multisampled resource — so the combination fails here, naming the conflict, rather than as an opaque
    // CreateCommittedResource error.
    // stream_scope::subresource wants only allow_subresource_stream, which carries no such restriction.
    if (usage.has(texture_usage::allow_region_stream))
    {
        CC_ASSERT(!usage.has(texture_usage::depth_stencil), "texture_usage::allow_region_stream is incompatible with "
                                                            "depth_stencil — stream a whole subresource with "
                                                            "allow_subresource_stream instead");
        CC_ASSERT(sample_count == 1, "texture_usage::allow_region_stream is incompatible with multisampling — stream a "
                                     "whole subresource with allow_subresource_stream instead");
    }
}
} // namespace sg
