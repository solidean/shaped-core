#include <clean-core/common/assert.hh>
#include <shaped-graphics/raw_texture.hh>

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

    // Shape invariants: array-ness / cube-ness / multisampling only combine with the dimensions that
    // support them across every backend.
    if (dimension == texture_dimension::d1 && (is_cube || sample_count != 1))
        return false;
    if (dimension == texture_dimension::d3 && (is_cube || array_layers.has_value() || sample_count != 1))
        return false;
    if (is_cube && dimension != texture_dimension::d2)
        return false;
    if (sample_count > 1 && (dimension != texture_dimension::d2 || mip_levels != 1))
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

    // Shape invariants: array-ness / cube-ness / multisampling only combine with the dimensions that
    // support them across every backend.
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
}
} // namespace sg
