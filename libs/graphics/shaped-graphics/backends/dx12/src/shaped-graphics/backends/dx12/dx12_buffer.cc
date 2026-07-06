// dx12_buffer: GPU buffer creation (committed + placed) and deferred-deletion destructor. The buffer
// type is otherwise header-only (ctor + fields).

#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>

namespace sg::backend::dx12
{
D3D12_RESOURCE_DESC buffer_resource_desc(cc::isize size_in_bytes, sg::buffer_usage usage)
{
    CC_ASSERT(size_in_bytes > 0, "buffer resource desc requires a positive size");

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = UINT64(size_in_bytes);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for buffers
    // Only a UAV (read-write storage) needs a creation flag; SRV / CBV / VBV / IBV / copy / indirect
    // are all allowed by default on a D3D12 buffer.
    desc.Flags = sg::has_flag(usage, sg::buffer_usage::readwrite_buffer) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                         : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

sg::access_barrier dx12_buffer::declare_access(sg::command_list_slot slot,
                                               sg::pipeline_stage_flags stages,
                                               sg::access_flags access) const
{
    return _access.lock(
        [&](access_tracking& t) -> sg::access_barrier
        {
            int const i = int(slot);
            CC_ASSERT(i >= 0, "declare_access with an invalid command_list_slot");
            while (t.slots.size() <= i)
                t.slots.push_back(access_slot{});
            auto& e = t.slots[i];
            if (!e.active)
            {
                e.active = true;
                e.state = t.canonical; // start from the committed state (general layout for buffers)
            }
            e.state.declare(stages, access); // layout defaults to general — buffers never transition
            return e.state.flush();
        });
}

void dx12_buffer::finalize_slot(sg::command_list_slot slot, bool promote) const
{
    _access.lock(
        [&](access_tracking& t)
        {
            int const i = int(slot);
            if (i >= t.slots.size() || !t.slots[i].active)
                return; // this list never touched the buffer
            auto& e = t.slots[i];
            CC_ASSERT(!e.state.has_pending_declares(), "a declared access was never flushed by a GPU op");
            if (promote)
                t.canonical = e.state; // committed state carries into the next command list
            // else: roll back to canonical. For buffers this emits nothing (layout is always general); the
            // revert transition + its hidden-cost warning arrive with textures.
            e.active = false;
            e.state = sg::resource_access_state{};
        });
}

void dx12_buffer::discard_slot(sg::command_list_slot slot) const
{
    _access.lock(
        [&](access_tracking& t)
        {
            int const i = int(slot);
            if (i >= t.slots.size())
                return;
            t.slots[i].active = false;
            t.slots[i].state = sg::resource_access_state{};
        });
}

void dx12_buffer::release_storage() const
{
    // Stage the GPU handle + finalizers for deletion once the current epoch retires. Empty buffers
    // (null resource) with no finalizers own nothing GPU-side; already-released ones no-op here.
    if (_resource || !_finalizers.empty())
    {
        dx12_expiring_resource expiring;
        expiring.resource = cc::move(_resource);
        expiring.finalizers = cc::move(_finalizers);
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

void dx12_buffer::on_expired() const
{
    release_storage();
}

dx12_buffer::~dx12_buffer()
{
    release_storage();
} // no-op if expire() already released the storage

cc::result<dx12_buffer_handle> dx12_context::create_dx12_buffer(cc::isize size_in_bytes,
                                                                sg::buffer_usage usage,
                                                                sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    ComPtr<ID3D12Resource> resource;
    sg::memory_heap_handle heap_handle; // held by the buffer for a placed resource; null when dedicated

    // Empty buffer: no allocation (D3D12 rejects a zero-width resource); null is the representation.
    if (size_in_bytes > 0)
    {
        D3D12_RESOURCE_DESC const desc = buffer_resource_desc(size_in_bytes, usage);

        // Created in COMMON: buffer copies rely on D3D12 implicit state promotion/decay (a buffer is
        // promoted from COMMON to COPY_DEST / COPY_SOURCE on use and decays back at ExecuteCommandLists),
        // so no explicit barriers are recorded for transfers.
        // TODO: a real per-resource barrier + state-tracking system will replace this (and enable, e.g.,
        // uploading then downloading the same buffer within one command list).
        if (alloc.is_placed())
        {
            // Placed: sub-allocate into the caller's heap at the offset its allocator picked.
            auto const dx_heap = std::dynamic_pointer_cast<dx12_memory_heap const>(alloc.heap);
            CC_ASSERT(dx_heap != nullptr, "memory_heap is not a dx12 memory_heap");
            CC_ASSERT(dx_heap->_heap != nullptr, "cannot place a buffer into an empty (size 0) heap");
            CC_ASSERT(alloc.offset >= 0, "placement offset must be non-negative");
            CC_ASSERT(alloc.offset + size_in_bytes <= dx_heap->size_in_bytes(), "placement exceeds the heap");
            if (HRESULT hr = _device->CreatePlacedResource(dx_heap->_heap.Get(), UINT64(alloc.offset), &desc,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
                FAILED(hr))
                return dx12_error(hr, "ID3D12Device::CreatePlacedResource failed");
            heap_handle = alloc.heap;
        }
        else
        {
            // Dedicated: a committed resource owns its own allocation.
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident: sg exposes no host-visible buffers.
            if (HRESULT hr = _device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
                FAILED(hr))
                return dx12_error(hr, "ID3D12Device::CreateCommittedResource failed");
        }
    }

    auto buffer = std::make_shared<dx12_buffer>(*this, current_epoch(), size_in_bytes, usage, cc::move(resource),
                                                cc::move(heap_handle));

    // A transient buffer is auto-expired when its epoch advances: register it so advance_epoch can flip
    // it (see dx12_epoch.cc). Weak, so holding the registration never keeps the buffer alive.
    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring.lock([&](cc::vector<std::weak_ptr<sg::buffer const>>& v) { v.push_back(buffer); });

    return dx12_buffer_handle(cc::move(buffer));
}
} // namespace sg::backend::dx12
