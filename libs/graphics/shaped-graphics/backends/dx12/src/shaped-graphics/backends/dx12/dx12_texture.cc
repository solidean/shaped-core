// dx12_texture: GPU texture creation (committed / dedicated) and deferred-deletion destructor. The
// type is otherwise header-only (ctor + fields).

#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_format.hh>
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
    desc.Format = to_dxgi_format(d.format);
    desc.SampleDesc.Count = UINT(d.sample_count);
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; // driver-chosen optimal tiling

    // Usage → creation flags. readonly_texture + copy need no flag (allowed by default); the
    // writable/attachment usages each add one. DENY_SHADER_RESOURCE is left off so a depth/RT texture
    // can also be sampled.
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (sg::has_flag(d.usage, sg::texture_usage::readwrite_texture))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (sg::has_flag(d.usage, sg::texture_usage::render_target))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (sg::has_flag(d.usage, sg::texture_usage::depth_stencil))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    return desc;
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

    // Stage the GPU handle + finalizers for deletion once the current epoch retires. Already-released
    // textures own nothing here and no-op.
    if (_resource || !_finalizers.empty())
    {
        dx12_expiring_resource expiring;
        expiring.resource = cc::move(_resource);
        expiring.finalizers = cc::move(_finalizers);
        // Hold the storage until any in-flight async copy queue upload that references it has finished,
        // even past the direct-queue epoch retire (mirrors dx12_buffer).
        expiring.copy_wait = dx12_copy_fence_value(_pending_async_upload_value.load(std::memory_order_acquire));
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

void dx12_texture::on_expired() const
{
    release_storage();
}

dx12_texture::~dx12_texture()
{
    release_storage();
} // no-op if expire() already released the storage

cc::result<dx12_texture_handle> dx12_context::create_dx12_texture(sg::texture_description const& desc,
                                                                  sg::allocation_info const& alloc)
{
    // Validate the shape contract before any fallible GPU work, so a bad desc asserts at the entry point
    // rather than surfacing as a CreateCommittedResource driver error.
    desc.assert_valid();

    // Dedicated committed resources only for now — placed textures need a texture-capable memory_heap
    // (the current heap is buffers-only). The transient scope also routes here with a dedicated alloc.
    CC_ASSERT(alloc.is_dedicated(), "placed textures are not supported yet (texture-capable heap is future work)");

    D3D12_RESOURCE_DESC const rdesc = texture_resource_desc(desc);

    // Created in COMMON. Textures are not usable in command lists yet (no layout tracking), so no initial
    // transition is recorded; the layout/barrier system arrives with texture command-list support.
    ComPtr<ID3D12Resource> resource;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident; sg exposes no host-visible textures
    if (HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, IID_PPV_ARGS(&resource));
        FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommittedResource (texture) failed");

    auto texture = std::make_shared<dx12_texture>(*this, current_epoch(), desc, cc::move(resource));

    // A transient texture is auto-expired when its epoch advances: register it so advance_epoch can flip
    // it (see dx12_epoch.cc). Weak, so holding the registration never keeps the texture alive.
    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring_textures.lock([&](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
                                          { v.push_back(texture); });

    return dx12_texture_handle(cc::move(texture));
}
} // namespace sg::backend::dx12
