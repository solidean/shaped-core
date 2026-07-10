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
    // are all allowed by default on a D3D12 buffer. Acceleration-structure *storage* is a UAV-flavored
    // buffer too (D3D12 requires ALLOW_UNORDERED_ACCESS for it) — plus it must be *created* in the
    // RAYTRACING_ACCELERATION_STRUCTURE state (see accel_structure_initial_state / dx12_context create).
    bool const needs_uav = sg::has_flag(usage, sg::buffer_usage::readwrite_buffer)
                        || sg::has_flag(usage, sg::buffer_usage::accel_structure_storage);
    desc.Flags = needs_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

void dx12_buffer::declare_access(sg::command_list_slot slot, sg::pipeline_stage_flags stages, sg::access_flags access) const
{
    _access.lock(
        [&](access_tracking& t)
        {
            int const i = int(slot);
            CC_ASSERT(i >= 0, "declare_access with an invalid command_list_slot");
            while (t.slots.size() <= i)
                t.slots.push_back(access_slot{});
            auto& e = t.slots[i];
            e.active = true; // a fresh state on first touch (access_slot{} default); no between-lists seed
            e.state.declare(stages, access); // accumulate only — layout defaults to general (buffers never transition)
        });
}

bool dx12_buffer::mark_pending_barrier(sg::command_list_slot slot) const
{
    return _access.lock(
        [&](access_tracking& t) -> bool
        {
            // Only ever called right after declare_access, so the slot exists and is active.
            int const i = int(slot);
            CC_ASSERT(i < t.slots.size() && t.slots[i].active, "mark_pending_barrier before declare_access");
            auto& e = t.slots[i];
            if (e.pending_barrier)
                return false;
            e.pending_barrier = true;
            return true;
        });
}

sg::access_barrier dx12_buffer::flush_access(sg::command_list_slot slot) const
{
    return _access.lock(
        [&](access_tracking& t) -> sg::access_barrier
        {
            // Only ever called for a slot that was just declared (so it exists and is active).
            int const i = int(slot);
            CC_ASSERT(i < t.slots.size() && t.slots[i].active, "flush_access of a buffer this list never declared");
            t.slots[i].pending_barrier = false; // this op's declares are being flushed
            return t.slots[i].state.flush();
        });
}

bool dx12_buffer::mark_recorded(sg::command_list_slot slot) const
{
    return _access.lock(
        [&](access_tracking& t) -> bool
        {
            int const i = int(slot);
            CC_ASSERT(i >= 0, "mark_recorded with an invalid command_list_slot");
            while (t.slots.size() <= i)
                t.slots.push_back(access_slot{});
            auto& e = t.slots[i];
            if (e.recorded)
                return false;
            e.recorded = true;
            return true;
        });
}

void dx12_buffer::finalize_slot(sg::command_list_slot slot) const
{
    _access.lock(
        [&](access_tracking& t)
        {
            // Only ever called for a buffer this list touched (declare_access set the slot active), so it
            // exists and is active — see the command list's submit path. A buffer has no layout and no
            // between-lists state, so finalize just frees the slot (its state is discarded, not carried).
            int const i = int(slot);
            CC_ASSERT(i < t.slots.size() && t.slots[i].active, "finalize of a buffer this list never touched");
            CC_ASSERT(!t.slots[i].state.has_pending_declares(), "a declared access was never flushed by a GPU op");
            t.slots[i] = access_slot{}; // clears active + recorded + pending_barrier + state
        });
}

void dx12_buffer::discard_slot(sg::command_list_slot slot) const
{
    _access.lock(
        [&](access_tracking& t)
        {
            // Like finalize, only ever called for a buffer this list touched (its slot is active).
            int const i = int(slot);
            CC_ASSERT(i < t.slots.size() && t.slots[i].active, "discard of a buffer this list never touched");
            t.slots[i] = access_slot{}; // clears active + recorded + pending_barrier + state
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
        // Gate release on the async copy queue too: an in-flight upload to this buffer may still reference
        // the resource after its epoch (direct queue) has retired. This is the buffer's highest pending
        // async value; deferred deletion holds the resource until the copy fence reaches it.
        expiring.copy_wait = dx12_copy_fence_value(_pending_async_upload_value.load(std::memory_order_acquire));
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
        // Exception: acceleration-structure storage MUST be created in the RAYTRACING_ACCELERATION_STRUCTURE
        // state and stays there for its whole life (D3D12 forbids transitioning an AS resource); the build's
        // accel_write/accel_read barriers are same-state UAV-style ordering, not layout transitions.
        D3D12_RESOURCE_STATES const initial_state = sg::has_flag(usage, sg::buffer_usage::accel_structure_storage)
                                                      ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                                      : D3D12_RESOURCE_STATE_COMMON;
        if (alloc.is_placed())
        {
            // Placed: sub-allocate into the caller's heap at the offset its allocator picked.
            auto const dx_heap = std::dynamic_pointer_cast<dx12_memory_heap const>(alloc.heap);
            CC_ASSERT(dx_heap != nullptr, "memory_heap is not a dx12 memory_heap");
            CC_ASSERT(dx_heap->_heap != nullptr, "cannot place a buffer into an empty (size 0) heap");
            CC_ASSERT(alloc.offset >= 0, "placement offset must be non-negative");
            CC_ASSERT(alloc.offset + size_in_bytes <= dx_heap->size_in_bytes(), "placement exceeds the heap");
            if (HRESULT hr = _device->CreatePlacedResource(dx_heap->_heap.Get(), UINT64(alloc.offset), &desc,
                                                           initial_state, nullptr, IID_PPV_ARGS(&resource));
                FAILED(hr))
                return dx12_error(hr, "ID3D12Device::CreatePlacedResource failed");
            heap_handle = alloc.heap;
        }
        else
        {
            // Dedicated: a committed resource owns its own allocation.
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident: sg exposes no host-visible buffers.
            if (HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state,
                                                              nullptr, IID_PPV_ARGS(&resource));
                FAILED(hr))
                return dx12_error(hr, "ID3D12Device::CreateCommittedResource failed");
        }
    }

    auto buffer = std::make_shared<dx12_buffer>(*this, current_epoch(), size_in_bytes, usage, cc::move(resource),
                                                cc::move(heap_handle));

    // A transient buffer is auto-expired when its epoch advances: register it so advance_epoch can flip
    // it (see dx12_epoch.cc). Weak, so holding the registration never keeps the buffer alive.
    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring.lock([&](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v) { v.push_back(buffer); });

    return dx12_buffer_handle(cc::move(buffer));
}
} // namespace sg::backend::dx12
