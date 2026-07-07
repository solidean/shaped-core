#include <clean-core/common/assert.hh>
#include <shaped-graphics/raw_texture.hh>

namespace sg
{
raw_texture::~raw_texture() = default;

raw_texture::raw_texture(texture_description const& desc) : _desc(desc)
{
    CC_ASSERT(desc.format != pixel_format::undefined, "texture needs a concrete pixel_format");
    CC_ASSERT(desc.width >= 1 && desc.height >= 1 && desc.depth >= 1, "texture extents must be >= 1");
    CC_ASSERT(desc.mip_levels >= 1, "texture needs at least one mip level");
    CC_ASSERT(desc.sample_count >= 1, "sample_count must be >= 1 (1 = not multisampled)");
    CC_ASSERT(!desc.array_layers.has_value() || desc.array_layers.value() >= 1, "array_layers, if set, must be >= 1");

    // Shape invariants: array-ness / cube-ness / multisampling only combine with the dimensions that
    // support them across every backend.
    if (desc.dimension == texture_dimension::d1)
        CC_ASSERT(!desc.is_cube && desc.sample_count == 1, "1D textures are neither cube nor multisampled");
    if (desc.dimension == texture_dimension::d3)
        CC_ASSERT(!desc.is_cube && !desc.array_layers.has_value() && desc.sample_count == 1,
                  "3D textures are neither arrayed, cube, nor multisampled");
    if (desc.is_cube)
        CC_ASSERT(desc.dimension == texture_dimension::d2, "cube textures are 2D-faced");
    if (desc.sample_count > 1)
    {
        CC_ASSERT(desc.dimension == texture_dimension::d2, "multisampling is 2D only");
        CC_ASSERT(desc.mip_levels == 1, "multisampled textures have a single mip level");
    }
}
} // namespace sg
