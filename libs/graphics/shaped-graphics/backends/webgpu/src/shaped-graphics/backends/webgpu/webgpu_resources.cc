// webgpu_buffer, webgpu_texture and webgpu_memory_heap: creation, views and release.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>

namespace sg::backend::webgpu
{
webgpu_buffer::webgpu_buffer(webgpu_context& ctx, isize size_in_bytes, sg::buffer_usages usage, wgpu_buffer buffer, u64 serial)
  : sg::raw_buffer(size_in_bytes, usage), _ctx(ctx), _live(ctx), _buffer(cc::move(buffer)), _serial(serial)
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
    // A transient buffer goes to the next epoch's claimable side rather than away: the same request is likely to come.
    if (_transient && _buffer)
        _ctx.offer_buffer(_size_in_bytes, wgpuBufferGetUsage(_buffer.get()), cc::move(_buffer), _serial);
    else
        expiring.buffer = cc::move(_buffer);
    _buffer = {};
    expiring.finalizers = cc::move(_finalizers);
    _finalizers.clear();
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

std::shared_ptr<webgpu_buffer> webgpu_context::register_if_transient(std::shared_ptr<webgpu_buffer> buffer,
                                                                     sg::lifetime_scope scope)
{
    if (scope == sg::lifetime_scope::transient)
    {
        // A buffer the CPU maps has a mapping state, which another handle must not inherit.
        if ((wgpuBufferGetUsage(buffer->raw()) & (WGPUBufferUsage_MapRead | WGPUBufferUsage_MapWrite)) == 0)
            buffer->mark_transient();
        _transient_buffers.push_back(buffer);
    }
    return buffer;
}

cc::result<webgpu_buffer_handle> webgpu_context::create_webgpu_buffer(isize size_in_bytes,
                                                                      sg::buffer_usages usage,
                                                                      sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    assert_on_device_thread();
    CC_ASSERT(!usage.has(sg::buffer_usage::accel_structure_storage), "webgpu keeps every acceleration structure in its "
                                                                     "own pool, so no buffer carries "
                                                                     "accel_structure_storage");

    // A placement is ignored: WebGPU has no heaps, and the stub heap only ever sized one.
    auto wgpu_usage = to_wgpu_buffer_usage(usage);

    // WebGPU refuses a buffer with no usage at all, and sg's empty set means no operation may touch it, which a
    // copy-destination usage nobody records against honours.
    if (wgpu_usage == WGPUBufferUsage_None)
        wgpu_usage = WGPUBufferUsage_CopyDst;

    // The same transient request as one in the last epoch takes that buffer back.
    if (alloc.scope == sg::lifetime_scope::transient)
        if (auto claimed = claim_buffer(size_in_bytes, wgpu_usage); claimed.buffer)
        {
            note_handed_out(claimed.serial);
            return webgpu_buffer_handle(register_if_transient(
                std::make_shared<webgpu_buffer>(*this, size_in_bytes, usage, cc::move(claimed.buffer), claimed.serial),
                alloc.scope));
        }

    // What the object is, in its label: a browser names the label in every error about it, and sg has no names of its own.
    auto const label = cc::format("sg buffer {} B, usage {}", size_in_bytes, u32(wgpu_usage));
    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu(label),
        .usage = wgpu_usage,
        .size = u64(align_up(size_in_bytes, buffer_word_bytes)),
        .mappedAtCreation = WGPU_FALSE,
    };
    auto buffer = wgpu_buffer(wgpuDeviceCreateBuffer(device(), &desc));

    // An allocation WebGPU cannot make still hands back an object, invalid, and says so through the error callback.
    // Only a null object is a failure here.
    if (!buffer)
        return cc::error("wgpuDeviceCreateBuffer returned no buffer");

    ++_created.buffers;
    auto const serial = next_resource_serial();
    if (alloc.scope == sg::lifetime_scope::transient)
        note_handed_out(serial);
    return webgpu_buffer_handle(register_if_transient(
        std::make_shared<webgpu_buffer>(*this, size_in_bytes, usage, cc::move(buffer), serial), alloc.scope));
}

webgpu_texture::webgpu_texture(webgpu_context& ctx,
                               sg::texture_description const& desc,
                               std::shared_ptr<webgpu_texture_storage> storage,
                               bool owned)
  : sg::raw_texture(desc), _ctx(ctx), _live(ctx), _storage(cc::move(storage)), _owned(owned)
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
    if (_storage == nullptr && _finalizers.empty())
        return;
    webgpu_expiring_resource expiring;
    // A transient texture goes to the next epoch's claimable side, its views with it.
    if (_owned && _transient && _storage != nullptr)
        _ctx.offer_texture(_desc, cc::move(_storage));
    else if (_owned && _storage != nullptr)
        expiring.texture = cc::move(_storage->texture);
    _storage = nullptr;
    expiring.finalizers = cc::move(_finalizers);
    _finalizers.clear();
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

wgpu_texture_view webgpu_texture::create_view(sg::texture_view_dimension dimension,
                                              sg::pixel_format format,
                                              sg::subresource_range const& range) const
{
    CC_ASSERT(_storage != nullptr && _storage->texture, "a view of an expired texture");
    auto const view_format = format == sg::pixel_format::undefined ? _desc.format : format;

    auto& storage = *_storage;
    auto const now = _ctx.current_epoch_number();
    for (auto& cached : storage.views)
        if (cached.dimension == dimension && cached.format == view_format && cached.range == range)
        {
            cached.last_used_epoch = now;
            return cached.view;
        }

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
    auto view = wgpu_texture_view(wgpuTextureCreateView(storage.texture.get(), &desc));
    _ctx.count_created_view();

    // The whole texture in its own format stays; any other view goes once unused for two epochs (advance_reuse).
    auto const layers
        = _desc.dimension == sg::texture_dimension::d3 ? 1 : _desc.array_layers.value_or(1) * (_desc.is_cube ? 6 : 1);
    auto const whole = view_format == _desc.format
                    && range
                           == sg::subresource_range({.start = 0, .end = _desc.mip_levels}, {.start = 0, .end = layers},
                                                    {.start = 0, .end = sg::format_aspect_count(_desc.format)});
    storage.views.push_back(
        {.dimension = dimension, .format = view_format, .range = range, .view = view, .last_used_epoch = now, .whole = whole});
    if (!whole && !storage.swept)
    {
        storage.swept = true;
        _ctx.sweep_views_of(_storage);
    }
    return view;
}

void webgpu_texture::release_cached_view(sg::texture_view_dimension dimension,
                                         sg::pixel_format format,
                                         sg::subresource_range const& range) const
{
    if (_storage == nullptr)
        return;
    auto const view_format = format == sg::pixel_format::undefined ? _desc.format : format;
    auto& views = _storage->views;
    for (isize i = 0; i < views.size(); ++i)
        if (views[i].dimension == dimension && views[i].format == view_format && views[i].range == range)
        {
            views.remove_at_unordered(i);
            return;
        }
}

void webgpu_texture::release_cached_views() const
{
    if (_storage != nullptr)
        _storage->views.clear();
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
    {
        texture->mark_transient();
        _transient_textures.push_back(texture);
    }
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

    // The two optional format features are adapter facts: a device without one refuses the format here rather than with a later validation error.
    if (sg::is_compressed_format(desc.format) && !wgpuDeviceHasFeature(device(), WGPUFeatureName_TextureCompressionBC))
        return cc::error("a BC-compressed texture needs the texture-compression-bc feature, which this device lacks");
    if (desc.format == sg::pixel_format::depth32_float_stencil8
        && !wgpuDeviceHasFeature(device(), WGPUFeatureName_Depth32FloatStencil8))
        return cc::error("depth32_float_stencil8 needs the depth32float-stencil8 feature, which this device lacks");

    // The same transient request as one in the last epoch takes that texture back, with the views made of it.
    if (alloc.scope == sg::lifetime_scope::transient)
        if (auto storage = claim_texture(desc); storage != nullptr)
        {
            note_handed_out(storage->serial);
            return webgpu_texture_handle(register_if_transient(
                std::make_shared<webgpu_texture>(*this, desc, cc::move(storage), true), alloc.scope));
        }

    // A 1D texture becomes 2D of height 1; a cube is six layers per cube; a 3D texture keeps its depth.
    auto const height = desc.dimension == sg::texture_dimension::d1 ? 1u : u32(desc.height);
    auto layers = u32(desc.array_layers.value_or(1));
    if (desc.is_cube)
        layers *= 6;
    auto const depth_or_layers = desc.dimension == sg::texture_dimension::d3 ? u32(desc.depth) : layers;

    auto const view_formats = view_formats_of(desc.format);
    auto const label = cc::format(
        "sg texture {}x{}x{}, format {}{}, {} mips, usage {}", desc.width, height, depth_or_layers, int(desc.format),
        sg::is_compressed_format(desc.format) ? " (BC)" : "", desc.mip_levels, u32(to_wgpu_texture_usage(desc.usage)));
    auto const wgpu_desc = WGPUTextureDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu(label),
        .usage = to_wgpu_texture_usage(desc.usage),
        .dimension = to_wgpu_texture_dimension(desc.dimension),
        .size = {.width = u32(desc.width), .height = height, .depthOrArrayLayers = depth_or_layers},
        .format = to_wgpu_format(desc.format),
        .mipLevelCount = u32(desc.mip_levels),
        .sampleCount = u32(desc.sample_count),
        .viewFormatCount = size_t(view_formats.size()),
        .viewFormats = view_formats.empty() ? nullptr : view_formats.data(),
    };
    auto texture = wgpu_texture(wgpuDeviceCreateTexture(device(), &wgpu_desc));
    if (!texture)
        return cc::error("wgpuDeviceCreateTexture returned no texture");

    ++_created.textures;
    auto storage = std::make_shared<webgpu_texture_storage>();
    storage->texture = cc::move(texture);
    storage->serial = next_resource_serial();
    if (alloc.scope == sg::lifetime_scope::transient)
        note_handed_out(storage->serial);
    return webgpu_texture_handle(
        register_if_transient(std::make_shared<webgpu_texture>(*this, desc, cc::move(storage), true), alloc.scope));
}

sg::memory_requirements webgpu_memory_heap::query_buffer_requirements(isize size_in_bytes, sg::buffer_usages) const
{
    return sg::memory_requirements{
        .alignment_in_bytes = buffer_word_bytes,
        .size_in_bytes = align_up(size_in_bytes, buffer_word_bytes),
    };
}
} // namespace sg::backend::webgpu
