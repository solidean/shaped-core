#include <clean-core/common/assert.hh>
#include <shaped-graphics/barrier/resource_access.hh>
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

    if (initial_layout.has_value()
        && (initial_layout.value() == texture_layout::undefined || initial_layout.value() == texture_layout::present))
        return false;

    return true;
}

texture_layout texture_description::resolved_initial_layout() const
{
    if (initial_layout.has_value())
        return initial_layout.value();

    // Most-specific usage first: the resting layout should be the one the texture spends its life in, and a texture
    // that is both a render target and sampled is a render target that happens to be read.
    // `general` is deliberately last rather than the default — it is the one layout drivers cannot compress, and a
    // resting layout is permanent in the sense that nothing ever transitions back to it on its own.
    if (usage.has(texture_usage::depth_stencil))
        return texture_layout::depth_readwrite;
    if (usage.has(texture_usage::render_target))
        return texture_layout::render_target;
    if (usage.has(texture_usage::readwrite_texture))
        return texture_layout::shader_readwrite;
    if (usage.has(texture_usage::readonly_texture))
        return texture_layout::shader_readonly;
    if (usage.has(texture_usage::copy_dst))
        return texture_layout::copy_dst;
    if (usage.has(texture_usage::copy_src))
        return texture_layout::copy_src;

    return texture_layout::general;
}

void texture_description::assert_valid() const
{
    CC_ASSERT(format != pixel_format::undefined, "texture needs a concrete pixel_format");

    // Caught here rather than at the view, because the usage is what makes the view reachable at all.
    // A typed UAV over an sRGB or block-compressed format is rejected by both backends, and D3D12 answers a
    // CreateUnorderedAccessView it does not like by REMOVING THE DEVICE — a failure with no return code, arriving a
    // frame later, nowhere near the description that caused it.
    // A renderable format that cannot carry a UAV is written through a raster pass instead.
    CC_ASSERT(!usage.has(texture_usage::readwrite_texture) || supports_typed_uav(format),
              "readwrite_texture usage needs a format that can carry a typed UAV — sRGB, block-compressed and depth "
              "formats cannot");
    CC_ASSERT(width >= 1 && height >= 1 && depth >= 1, "texture extents must be >= 1");
    CC_ASSERT(mip_levels >= 1, "texture needs at least one mip level");
    CC_ASSERT(sample_count >= 1, "sample_count must be >= 1 (1 = not multisampled)");
    CC_ASSERT(!array_layers.has_value() || array_layers.value() >= 1, "array_layers, if set, must be >= 1");
    CC_ASSERT(!initial_layout.has_value() || initial_layout.value() != texture_layout::undefined,
              "texture_layout::undefined is not a resting layout — leave initial_layout unset to derive one");
    CC_ASSERT(!initial_layout.has_value() || initial_layout.value() != texture_layout::present,
              "texture_layout::present belongs to a swapchain image, not to a created texture");

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
