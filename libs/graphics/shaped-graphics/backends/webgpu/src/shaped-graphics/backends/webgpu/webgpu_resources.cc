// webgpu_buffer, webgpu_texture and webgpu_memory_heap: creation, views and release.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>

namespace sg::backend::webgpu
{
webgpu_buffer::webgpu_buffer(webgpu_context& ctx, isize size_in_bytes, sg::buffer_usages usage, wgpu_buffer buffer)
  : sg::raw_buffer(size_in_bytes, usage), _ctx(ctx), _buffer(cc::move(buffer))
{
}

webgpu_buffer::~webgpu_buffer()
{
    release_storage();
}

void webgpu_buffer::on_expired() const
{
    release_storage();
}

void webgpu_buffer::release_storage() const
{
    if (!_buffer && _finalizers.empty())
        return;
    webgpu_expiring_resource expiring;
    expiring.buffer = cc::move(_buffer);
    expiring.finalizers = cc::move(_finalizers);
    _finalizers.clear();
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

std::shared_ptr<webgpu_buffer> webgpu_context::register_if_transient(std::shared_ptr<webgpu_buffer> buffer,
                                                                     sg::lifetime_scope scope)
{
    if (scope == sg::lifetime_scope::transient)
        _transient_buffers.push_back(buffer);
    return buffer;
}

cc::result<webgpu_buffer_handle> webgpu_context::create_webgpu_buffer(isize size_in_bytes,
                                                                      sg::buffer_usages usage,
                                                                      sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    assert_on_device_thread();
    CC_ASSERT(!usage.has_any(sg::buffer_usage::accel_structure_storage | sg::buffer_usage::accel_structure_build_input),
              "webgpu has no ray tracing, so no acceleration-structure buffer usage");

    // A placement is ignored: WebGPU has no heaps, and the stub heap only ever sized one.
    auto wgpu_usage = to_wgpu_buffer_usage(usage);

    // WebGPU refuses a buffer with no usage at all, and sg's empty set means no operation may touch it, which a
    // copy-destination usage nobody records against honours.
    if (wgpu_usage == WGPUBufferUsage_None)
        wgpu_usage = WGPUBufferUsage_CopyDst;

    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg buffer"),
        .usage = wgpu_usage,
        .size = u64(align_up(size_in_bytes, buffer_word_bytes)),
        .mappedAtCreation = WGPU_FALSE,
    };
    auto buffer = wgpu_buffer(wgpuDeviceCreateBuffer(_device.get(), &desc));

    // An allocation WebGPU cannot make still hands back an object, invalid, and says so through the error callback.
    // Only a null object is a failure here.
    if (!buffer)
        return cc::error("wgpuDeviceCreateBuffer returned no buffer");

    return webgpu_buffer_handle(register_if_transient(
        std::make_shared<webgpu_buffer>(*this, size_in_bytes, usage, cc::move(buffer)), alloc.scope));
}

webgpu_texture::webgpu_texture(webgpu_context& ctx, sg::texture_description const& desc, wgpu_texture texture, bool owned)
  : sg::raw_texture(desc), _ctx(ctx), _texture(cc::move(texture)), _owned(owned)
{
}

webgpu_texture::~webgpu_texture()
{
    release_storage();
}

void webgpu_texture::on_expired() const
{
    release_storage();
}

void webgpu_texture::release_storage() const
{
    if (!_texture && _finalizers.empty())
        return;
    webgpu_expiring_resource expiring;
    if (_owned)
        expiring.texture = cc::move(_texture);
    _texture = {};
    expiring.finalizers = cc::move(_finalizers);
    _finalizers.clear();
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

wgpu_texture_view webgpu_texture::create_view(sg::texture_view_dimension dimension,
                                              sg::pixel_format format,
                                              sg::subresource_range const& range) const
{
    auto const view_format = format == sg::pixel_format::undefined ? _desc.format : format;
    auto const desc = WGPUTextureViewDescriptor{
        .nextInChain = nullptr,
        .label = WGPU_STRING_VIEW_INIT,
        .format = to_wgpu_format(view_format),
        .dimension = to_wgpu_view_dimension(dimension),
        .baseMipLevel = u32(range.mip_range.start),
        .mipLevelCount = u32(range.mip_range.end - range.mip_range.start),
        .baseArrayLayer = u32(range.array_range.start),
        .arrayLayerCount = u32(range.array_range.end - range.array_range.start),
        .aspect = to_wgpu_aspect(_desc.format, range.aspect_range),
        .usage = WGPUTextureUsage_None,
    };
    return wgpu_texture_view(wgpuTextureCreateView(_texture.get(), &desc));
}

cc::span<WGPUTextureFormat const> view_formats_of(sg::pixel_format format)
{
    static constexpr WGPUTextureFormat rgba8_srgb[] = {WGPUTextureFormat_RGBA8UnormSrgb};
    static constexpr WGPUTextureFormat rgba8[] = {WGPUTextureFormat_RGBA8Unorm};
    static constexpr WGPUTextureFormat bgra8_srgb[] = {WGPUTextureFormat_BGRA8UnormSrgb};
    static constexpr WGPUTextureFormat bgra8[] = {WGPUTextureFormat_BGRA8Unorm};
    switch (format)
    {
    case sg::pixel_format::rgba8_unorm:
        return rgba8_srgb;
    case sg::pixel_format::rgba8_unorm_srgb:
        return rgba8;
    case sg::pixel_format::bgra8_unorm:
        return bgra8_srgb;
    case sg::pixel_format::bgra8_unorm_srgb:
        return bgra8;
    default:
        return {};
    }
}

std::shared_ptr<webgpu_texture> webgpu_context::register_if_transient(std::shared_ptr<webgpu_texture> texture,
                                                                      sg::lifetime_scope scope)
{
    if (scope == sg::lifetime_scope::transient)
        _transient_textures.push_back(texture);
    return texture;
}

cc::result<webgpu_texture_handle> webgpu_context::create_webgpu_texture(sg::texture_description const& desc,
                                                                        sg::allocation_info const& alloc)
{
    desc.assert_valid();
    assert_on_device_thread();
    CC_ASSERT(alloc.is_dedicated(), "placed textures (non-null memory_heap) are not supported");

    if (desc.sample_count != 1 && desc.sample_count != 4)
        return cc::error(cc::format("webgpu supports a sample count of 1 or 4, not {}", desc.sample_count));

    // A 1D texture becomes 2D of height 1; a cube is six layers per cube; a 3D texture keeps its depth.
    auto const height = desc.dimension == sg::texture_dimension::d1 ? 1u : u32(desc.height);
    auto layers = u32(desc.array_layers.value_or(1));
    if (desc.is_cube)
        layers *= 6;
    auto const depth_or_layers = desc.dimension == sg::texture_dimension::d3 ? u32(desc.depth) : layers;

    auto const view_formats = view_formats_of(desc.format);
    auto const wgpu_desc = WGPUTextureDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg texture"),
        .usage = to_wgpu_texture_usage(desc.usage),
        .dimension = to_wgpu_texture_dimension(desc.dimension),
        .size = {.width = u32(desc.width), .height = height, .depthOrArrayLayers = depth_or_layers},
        .format = to_wgpu_format(desc.format),
        .mipLevelCount = u32(desc.mip_levels),
        .sampleCount = u32(desc.sample_count),
        .viewFormatCount = size_t(view_formats.size()),
        .viewFormats = view_formats.empty() ? nullptr : view_formats.data(),
    };
    auto texture = wgpu_texture(wgpuDeviceCreateTexture(_device.get(), &wgpu_desc));
    if (!texture)
        return cc::error("wgpuDeviceCreateTexture returned no texture");

    return webgpu_texture_handle(
        register_if_transient(std::make_shared<webgpu_texture>(*this, desc, cc::move(texture), true), alloc.scope));
}

sg::memory_requirements webgpu_memory_heap::query_buffer_requirements(isize size_in_bytes, sg::buffer_usages) const
{
    return sg::memory_requirements{
        .alignment_in_bytes = buffer_word_bytes,
        .size_in_bytes = align_up(size_in_bytes, buffer_word_bytes),
    };
}
} // namespace sg::backend::webgpu
