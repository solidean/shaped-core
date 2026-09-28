#pragma once

#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/subresource.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/resource/views.hh>
#include <shaped-graphics/types.hh>

// Translations from sg's vocabulary to WebGPU's, for the parts every other file needs.

/// The layout of a texture copy that places rows through a buffer: rows at a multiple of 256 bytes.
struct sg::backend::webgpu::texel_copy_layout
{
    isize row_bytes = 0;    ///< tightly packed bytes per row, whole blocks
    isize padded_row = 0;   ///< row_bytes rounded up to copy_row_alignment
    isize rows = 0;         ///< rows per image, in blocks
    isize images = 0;       ///< depth slices
    isize packed_bytes = 0; ///< what sg hands over or expects back
    isize staged_bytes = 0; ///< what the staging buffer must hold: every row padded but the very last
};

namespace sg::backend::webgpu
{
[[nodiscard]] WGPUTextureFormat to_wgpu_format(sg::pixel_format f);
[[nodiscard]] WGPUBufferUsage to_wgpu_buffer_usage(sg::buffer_usages usage);
[[nodiscard]] WGPUTextureUsage to_wgpu_texture_usage(sg::texture_usages usage);

/// Every sg 1D texture is a WebGPU 2D texture of height 1, so only d3 differs from 2D.
/// WebGPU's own 1D textures allow one mip, no arrays and no storage or render use, which sg's 1D textures all need.
[[nodiscard]] WGPUTextureDimension to_wgpu_texture_dimension(sg::texture_dimension d);

/// The view dimension a shader binds, with 1D views read as 2D ones for the reason above.
[[nodiscard]] WGPUTextureViewDimension to_wgpu_view_dimension(sg::texture_view_dimension d);

/// The WebGPU aspect a positional aspect range of `format` names; `all` when it covers every plane.
[[nodiscard]] WGPUTextureAspect to_wgpu_aspect(sg::pixel_format format, cc::start_end aspect_range);

/// The aspect one subresource_index names, for a copy.
[[nodiscard]] WGPUTextureAspect to_wgpu_copy_aspect(sg::pixel_format format, sg::texture_aspect aspect);

/// The stages a binding is visible to.
/// An empty set means the binding never said, and then it is every stage it is legal in: WebGPU refuses a writable storage binding visible to the vertex stage.
[[nodiscard]] WGPUShaderStage to_wgpu_visibility(sg::shader_stages visibility, bool writable);

/// How the texels of `format` are sampled when a binding does not say.
[[nodiscard]] WGPUTextureSampleType default_sample_type(sg::pixel_format format);
[[nodiscard]] WGPUTextureSampleType to_wgpu_sample_type(sg::texture_sample_type t);
[[nodiscard]] WGPUSamplerBindingType to_wgpu_sampler_binding_type(sg::sampler_binding_type t);

[[nodiscard]] WGPUCompareFunction to_wgpu_compare(sg::compare_op op);

/// A sampler descriptor for `s`.
[[nodiscard]] WGPUSamplerDescriptor to_wgpu_sampler(sg::sampler const& s);

/// The sampler binding kind a sampler state needs when a binding does not say.
[[nodiscard]] WGPUSamplerBindingType default_sampler_binding_type(sg::sampler const& s);


/// The copy layout of `region` in `format`.
[[nodiscard]] texel_copy_layout texel_copy_layout_of(sg::pixel_format format, tg::vec3i size);

/// The extent a copy of `size` texels of `format` is recorded with: whole blocks, which a region running to a mip's edge needs.
/// WebGPU measures a block-compressed copy against the mip's physical size, rounded up to whole blocks, so this is always in range.
[[nodiscard]] inline WGPUExtent3D copy_extent_of(sg::pixel_format format, tg::vec3i size)
{
    auto const block = isize(sg::format_block_extent(format));
    return WGPUExtent3D{u32(align_up(size[0], block)), u32(align_up(size[1], block)), u32(size[2])};
}
} // namespace sg::backend::webgpu
