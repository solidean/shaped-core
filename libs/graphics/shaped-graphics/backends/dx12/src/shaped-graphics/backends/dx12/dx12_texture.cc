// dx12_texture: GPU texture creation, committed or placed, plus the deferred-deletion destructor.
// The type is otherwise header-only (ctor + fields).

#include <clean-core/common/profiling.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_format.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>

namespace sg::backend::dx12
{
D3D12_RESOURCE_DESC texture_resource_desc(sg::texture_description const& d)
{
    D3D12_RESOURCE_DESC desc = {};

    switch (d.dimension)
    {
    case sg::texture_dimension::d1:
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        break;
    case sg::texture_dimension::d2:
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        break;
    case sg::texture_dimension::d3:
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        break;
    }

    desc.Width = UINT64(d.width);
    desc.Height = UINT(d.height);

    // DepthOrArraySize is the depth for 3D, else the array-slice count (a cube is 6 slices per cube).
    UINT slices_or_depth = 0;
    if (d.dimension == sg::texture_dimension::d3)
        slices_or_depth = UINT(d.depth);
    else
    {
        int layers = d.array_layers.value_or(1);
        if (d.is_cube)
            layers *= 6;
        slices_or_depth = UINT(layers);
    }
    desc.DepthOrArraySize = UINT16(slices_or_depth);

    desc.MipLevels = UINT16(d.mip_levels);
    desc.Format = to_dxgi_resource_format(d.format, d.usage.has(sg::texture_usage::texture));
    desc.SampleDesc.Count = UINT(d.sample_count);
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; // driver-chosen optimal tiling
    // Usage → creation flags.
    // texture and copy need no flag, being allowed by default; the writable/attachment usages each add one.
    // DENY_SHADER_RESOURCE is left off, so a depth/RT texture can also be sampled.
    // can also be sampled.
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (d.usage.has(sg::texture_usage::image))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (d.usage.has(sg::texture_usage::render_target))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (d.usage.has(sg::texture_usage::depth_stencil))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    // Only stream_scope::region needs this: state is already tracked per subresource, so streaming a whole mip while
    // another is sampled costs nothing.
    // It is not free — it rules out depth/stencil and MSAA and can disable metadata compression — so it follows the
    // narrow flag only, never allow_subresource_stream.
    if (d.usage.has(sg::texture_usage::allow_region_stream))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    return desc;
}

void dx12_texture::acquire_completion_groups()
{
    // Mirrors dx12_buffer: a texture that can never be copied needs no timeline.
    if (usage().has(sg::texture_usage::copy_dst))
        _upload_group = _ctx._group_pool.acquire();
    if (usage().has(sg::texture_usage::copy_src))
        _download_group = _ctx._group_pool.acquire();
}

void dx12_texture::release_storage() const
{
    // Borrowed (swapchain) storage: DXGI owns the resource and the swapchain already waited for the GPU, so
    // drop our reference right now (deferred deletion would keep the back-buffer reference alive past the
    // ResizeBuffers/teardown that needs it gone).
    if (_borrowed)
    {
        _resource.Reset();
        for (auto& finalizer : _finalizers)
            finalizer();
        _finalizers.clear();
        return;
    }

    // Stage the GPU handle + finalizers for deletion once the current epoch retires.
    // An already-released texture owns nothing here and no-ops.
    if (_resource || !_finalizers.empty())
    {
        dx12_expiring_resource expiring;
        expiring.resource = cc::move(_resource);
        expiring.finalizers = cc::move(_finalizers);
        // Hold the storage until any in-flight async copy queue upload that references it has finished,
        // even past the direct-queue epoch retire (mirrors dx12_buffer).
        expiring.copy_wait
            = dx12_group_value{_upload_group, cc::max(_pending_async_upload_value.load(std::memory_order_acquire),
                                                      _pending_stream_copy_value.load(std::memory_order_acquire))};
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

void dx12_texture::on_expired() const
{
    release_storage();
}

dx12_texture::~dx12_texture()
{
    // a pending upload no list waits on can never be read now, so its context waits on nothing (upload_contexts.hh)
    if (_upload_group != nullptr)
        _upload_group->contexts.release_unawaited(_upload_group->awaited_value.load(std::memory_order_acquire));
    release_storage();
} // no-op if expire() already released the storage

cc::result<dx12_texture_handle> dx12_context::create_dx12_texture(sg::texture_description const& desc,
                                                                  sg::allocation_info const& alloc)
{
    CC_RECORD_ACCUM("sg.texture.count_allocated", cc::rec::unit_count, 1);

    // Validate the shape contract before any fallible GPU work, so a bad desc asserts at the entry point
    // rather than surfacing as a CreateCommittedResource driver error.
    desc.assert_valid();

    D3D12_RESOURCE_DESC const rdesc = texture_resource_desc(desc);

    // Created in COMMON.
    // A committed texture's tracker rests it there, and a recording list transitions out of it only when its first access needs another layout.
    // A placed one's tracker rests it in `undefined` instead, whatever D3D12 created it in: its first barrier then discards, which initializes it.
    ComPtr<ID3D12Resource> resource;
    if (alloc.is_placed())
    {
        auto const dx_heap = std::dynamic_pointer_cast<dx12_memory_heap const>(alloc.heap);
        CC_ASSERT(dx_heap != nullptr, "placement heap is not a dx12 heap");
        CC_ASSERT(dx_heap->contents() == sg::memory_heap_contents::textures, "a texture is placed in a textures heap");
        CC_ASSERT(alloc.offset >= 0 && alloc.offset + alloc.size_in_bytes <= dx_heap->size_in_bytes(),
                  "placement exceeds the heap");
        if (HRESULT hr = _device->CreatePlacedResource(dx_heap->_heap.Get(), UINT64(alloc.offset), &rdesc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
            FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreatePlacedResource (texture) failed");
    }
    else
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident; sg exposes no host-visible textures
        if (HRESULT hr = _device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rdesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
            FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreateCommittedResource (texture) failed");
    }

    auto texture = std::make_shared<dx12_texture>(*this, current_epoch(), desc, cc::move(resource), alloc.heap);

    // A transient texture is auto-expired when its epoch advances: register it so advance_epoch can flip
    // it (see dx12_epoch.cc). Weak, so holding the registration never keeps the texture alive.
    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring_textures.lock([&](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
                                          { v.push_back(texture); });

    return dx12_texture_handle(cc::move(texture));
}

cc::result<dx12_texture_handle> dx12_context::wrap_dx12_texture(ID3D12Resource* resource,
                                                                sg::texture_description const& desc)
{
    CC_ASSERT(resource != nullptr, "wrap_dx12_texture: resource is null");
    desc.assert_valid();

    auto const want = texture_resource_desc(desc);
    auto const have = resource->GetDesc();
    if (have.Dimension != want.Dimension || have.Width != want.Width || have.Height != want.Height
        || have.DepthOrArraySize != want.DepthOrArraySize || have.MipLevels != want.MipLevels
        || have.SampleDesc.Count != want.SampleDesc.Count)
        return cc::error(cc::format("wrap_dx12_texture: the resource is {}x{}x{} with {} mips and {} samples, the "
                                    "description {}x{}x{} with {} mips and {} samples",
                                    have.Width, have.Height, have.DepthOrArraySize, have.MipLevels,
                                    have.SampleDesc.Count, want.Width, want.Height, want.DepthOrArraySize,
                                    want.MipLevels, want.SampleDesc.Count));
    for (auto const flag : {D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS})
        if ((want.Flags & flag) != 0 && (have.Flags & flag) == 0)
            return cc::error(cc::format("wrap_dx12_texture: the description's usage needs resource flag 0x{:x}, "
                                        "which the resource was not created with",
                                        u32(flag)));

    // Tracked from COMMON, exactly as a texture sg created itself.
    auto texture = std::make_shared<dx12_texture>(*this, current_epoch(), desc, ComPtr<ID3D12Resource>(resource));
    return dx12_texture_handle(cc::move(texture));
}
} // namespace sg::backend::dx12
