// dx12_command_list: allocation, submission, and drop. The list type is header-only (ctor +
// fields); its create/submit/drop bodies live here. Allocators are epoch-gated (recycled once the
// epoch retires); see libs/graphics/shaped-graphics/docs/concepts/epochs.md.

#include <clean-core/string/print.hh>
#include <shaped-graphics/backend/access_inference.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
void dx12_command_list::track_buffer_access(dx12_buffer_handle const& buffer,
                                            sg::pipeline_stage_flags stages,
                                            sg::access_flags access)
{
    if (buffer->_resource == nullptr)
        return; // empty (size 0) buffer: no GPU storage, nothing to order

    // Forward cross-queue sync: if an async upload (ctx.upload) is still writing this buffer on the copy
    // queue, the direct queue must wait for that copy before this list runs. Fold its completion value into
    // _required_copy_wait (idempotent max); the single wait is issued at submit.
    cc::u64 const async_v = buffer->_pending_async_upload_value.load(std::memory_order_acquire);
    if (async_v > cc::u64(_required_copy_wait))
        _required_copy_wait = dx12_copy_fence_value(async_v);

    sg::access_barrier const barrier = buffer->declare_access(slot(), stages, access);
    if (barrier.needed)
        emit_buffer_barrier(_list.Get(), buffer->_resource.Get(), barrier);

    // Record once so its slot is finalized at submit/drop and it gets the reverse async-upload stamp at
    // submit (dedup — a buffer may be touched by many ops).
    for (auto const& h : _touched_buffers)
        if (h == buffer)
            return;
    _touched_buffers.push_back(buffer);
}

void dx12_command_list::track_texture_access(dx12_texture_handle const& texture,
                                             sg::subresource_range range,
                                             sg::pipeline_stage_flags stages,
                                             sg::access_flags access,
                                             sg::texture_layout layout)
{
    if (texture->_resource == nullptr)
        return; // no GPU storage, nothing to order

    // Declare over the range and emit the per-box layout transition the tracker asks for (precise
    // subresource barriers — undefined→copy-dest, copy-dest→shader-resource, and the like).
    for (auto const& sb : texture->declare_texture_access(slot(), range, stages, access, layout))
        emit_texture_barrier(_list.Get(), texture->_resource.Get(), sb.range, sb.barrier);

    // Record once so its slot is finalized at submit/drop (dedup — a texture may be touched by many ops).
    for (auto const& h : _touched_textures)
        if (h == texture)
            return;
    _touched_textures.push_back(texture);
}

void dx12_command_list::compute_bind_pipeline(sg::compute_pipeline const& pipeline)
{
    auto const* dp = dynamic_cast<dx12_compute_pipeline const*>(&pipeline);
    CC_ASSERT(dp != nullptr, "compute_pipeline is not a dx12 compute_pipeline");

    // The shader-visible heap must be set before any root descriptor table is bound.
    ID3D12DescriptorHeap* heaps[] = {_ctx._descriptor_heap.heap.Get()};
    _list->SetDescriptorHeaps(1, heaps);
    _list->SetComputeRootSignature(dp->layout->root_signature.Get());
    _list->SetPipelineState(dp->pipeline_state.Get());
}

void dx12_command_list::compute_bind_group(int set, sg::binding_group const& group)
{
    CC_ASSERT(set == 0, "only descriptor set 0 is supported yet");
    auto const* dg = dynamic_cast<dx12_binding_group const*>(&group);
    CC_ASSERT(dg != nullptr, "binding_group is not a dx12 binding_group");

    // Transient expiry tripwire: a transient group's descriptor slots are recycled after its epoch, so
    // binding one past that epoch would point the table at another epoch's descriptors.
    CC_ASSERT(!(dg->transient && dg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");

    // Remember the bound group so its views' accesses are declared at dispatch (the point work runs). The
    // forward async-upload wait for each bound buffer is folded in there too, via track_buffer_access.
    _bound_group = dg;
    _list->SetComputeRootDescriptorTable(0, dg->table_start);
}

void dx12_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");

    // Declare each bound resource's shader access before the dispatch: the tracker emits any intra-list
    // hazard barrier (e.g. a prior transfer_write → shader_read RAW, or a WAW between two dispatches).
    // Cross-list visibility rides on D3D12 decaying buffers to COMMON at ExecuteCommandLists.
    if (_bound_group != nullptr)
        for (auto const& view : _bound_group->hazard_views)
            if (view.buffer)
                track_buffer_access(view.buffer, sg::pipeline_stage_flags::compute, sg::shader_access_of(view.access));

    _list->Dispatch(UINT(x), UINT(y), UINT(z));
}

void dx12_command_list::compute_declare_array_buffer_access(cc::string_view binding_name,
                                                            cc::span<sg::array_buffer_access const> elements)
{
    CC_ASSERT(!binding_name.empty(), "declare_array_buffer_access requires a binding name");
    // Arrays / bindless are not auto-tracked: which elements a shader indexes (and how) can't be inferred,
    // so the caller declares it here. Applying it needs a binding-name → bound-resources reflection map and
    // an array binding path, neither of which exists yet — so there is nothing to record for now.
    // TODO: once array bindings land, declare each element's access on its buffer for the next dispatch.
    (void)elements;
}

void dx12_command_list::compute_declare_array_texture_access(cc::string_view binding_name,
                                                             cc::span<sg::array_texture_access const> elements)
{
    CC_ASSERT(!binding_name.empty(), "declare_array_texture_access requires a binding name");
    // Texture arrays are blocked on sg::texture (no texture resource exists yet). The API is in place; the
    // per-element layout + subresource declaration will be applied once textures + the array binding path land.
    CC_ASSERT(elements.empty(), "texture array access declaration is not implemented yet (no sg::texture)");
}

void dx12_command_list::upload_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                               cc::span<cc::byte const> data,
                                               cc::isize offset_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "upload target buffer is null");
    auto const dst = std::dynamic_pointer_cast<dx12_buffer const>(buffer);
    CC_ASSERT(dst != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!dst->is_expired(), "upload target is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + data.size() <= dst->size_in_bytes(), "upload range is out of "
                                                                                             "the buffer's bounds");
    if (data.empty())
        return;
    CC_ASSERT(sg::has_flag(dst->usage(), sg::buffer_usage::copy_dst), "upload target buffer must have "
                                                                      "buffer_usage::copy_dst");
    // Order this write against any prior use of the buffer in this list (precise, no bounce through COMMON)
    // and fold in the forward async-upload wait, then record the copy.
    track_buffer_access(dst, sg::pipeline_stage_flags::transfer, sg::access_flags::transfer_write);
    _ctx._upload_inline.upload_buffer(*this, *dst, data, offset_in_bytes);
}

sg::bytes_future dx12_command_list::download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                               cc::isize offset_in_bytes,
                                                               cc::isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "download source buffer is null");
    auto const src = std::dynamic_pointer_cast<dx12_buffer const>(buffer);
    CC_ASSERT(src != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!src->is_expired(), "download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(size_in_bytes >= 0, "download size must be non-negative");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + size_in_bytes <= src->size_in_bytes(),
              "download range is out of the buffer's bounds");
    if (size_in_bytes > 0)
    {
        CC_ASSERT(sg::has_flag(src->usage(), sg::buffer_usage::copy_src), "download source buffer must have "
                                                                          "buffer_usage::copy_src");
    }
    // Order this read against any prior write of the buffer in this list, and fold in the forward
    // async-upload wait, then record the readback copy.
    if (size_in_bytes > 0)
        track_buffer_access(src, sg::pipeline_stage_flags::transfer, sg::access_flags::transfer_read);
    return _ctx._download_inline.download_buffer(*this, *src, offset_in_bytes, size_in_bytes);
}

void dx12_command_list::copy_buffer_region(sg::raw_buffer_handle src,
                                           sg::raw_buffer_handle dst,
                                           cc::isize src_offset_in_bytes,
                                           cc::isize dst_offset_in_bytes,
                                           cc::isize size_in_bytes)
{
    CC_ASSERT(src != nullptr, "copy source buffer is null");
    CC_ASSERT(dst != nullptr, "copy dest buffer is null");
    auto const s = std::dynamic_pointer_cast<dx12_buffer const>(src);
    auto const d = std::dynamic_pointer_cast<dx12_buffer const>(dst);
    CC_ASSERT(s != nullptr && d != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!s->is_expired() && !d->is_expired(), "copy uses a transient buffer past its epoch (expired)");
    CC_ASSERT(size_in_bytes >= 0, "copy size must be non-negative");
    CC_ASSERT(src_offset_in_bytes >= 0 && src_offset_in_bytes + size_in_bytes <= s->size_in_bytes(),
              "copy source range is out of the buffer's bounds");
    CC_ASSERT(dst_offset_in_bytes >= 0 && dst_offset_in_bytes + size_in_bytes <= d->size_in_bytes(),
              "copy dest range is out of the buffer's bounds");
    if (size_in_bytes == 0)
        return;
    CC_ASSERT(sg::has_flag(s->usage(), sg::buffer_usage::copy_src), "copy source buffer must have "
                                                                    "buffer_usage::copy_src");
    CC_ASSERT(sg::has_flag(d->usage(), sg::buffer_usage::copy_dst), "copy dest buffer must have "
                                                                    "buffer_usage::copy_dst");
    // Same-buffer copy: the source and destination ranges must not overlap.
    bool const same_resource = s->_resource.Get() == d->_resource.Get();
    if (same_resource)
        CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= src_offset_in_bytes
                      || src_offset_in_bytes + size_in_bytes <= dst_offset_in_bytes,
                  "source and destination ranges overlap in a same-buffer copy");

    // Order the copy against prior use of each buffer. A self-copy reads and writes one resource, so it is
    // declared as a single combined access (one barrier); distinct buffers are ordered independently.
    if (same_resource)
        track_buffer_access(s, sg::pipeline_stage_flags::transfer,
                            sg::access_flags::transfer_read | sg::access_flags::transfer_write);
    else
    {
        track_buffer_access(s, sg::pipeline_stage_flags::transfer, sg::access_flags::transfer_read);
        track_buffer_access(d, sg::pipeline_stage_flags::transfer, sg::access_flags::transfer_write);
    }

    _list->CopyBufferRegion(d->_resource.Get(), UINT64(dst_offset_in_bytes), s->_resource.Get(),
                            UINT64(src_offset_in_bytes), UINT64(size_in_bytes));
}

cc::result<std::unique_ptr<dx12_command_list>> dx12_context::create_dx12_command_list()
{
    // Single DIRECT queue for now; the pool is per-queue-ready for the copy/compute/video queues to come.
    auto constexpr queue = D3D12_COMMAND_LIST_TYPE_DIRECT;
    auto acquired = _cmd_pool.acquire_command_list(queue);
    CC_RETURN_IF_ERROR(acquired);

    _open_command_lists.fetch_add(1, std::memory_order_relaxed); // must reach 0 before the epoch can advance
    // Left open (recording); submit closes it. Stamped with the epoch it must be submitted/dropped in, plus
    // an access-tracking slot that keys its private per-resource state (released on submit/drop).
    return std::make_unique<dx12_command_list>(*this, current_epoch(), _command_list_slots.acquire(), queue,
                                               cc::move(acquired.value().allocator.allocator),
                                               cc::move(acquired.value().list));
}

sg::submission_token dx12_context::submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in (it cannot span epochs)");

    // Finalize access tracking before closing. `promote` when this is the only open list: its final state
    // becomes the resources' committed state (the one case that may leave a texture in a new layout);
    // otherwise each resource rolls back to its entry state. For buffers this is a no-op (layout always
    // general); for textures the rollback returns transitions back to the entry layout, recorded here
    // before Close, plus a hidden-cost warning.
    bool const promote = _command_list_slots.live_count() == 1;
    for (auto const& b : cmd->_touched_buffers)
        b->finalize_slot(cmd->slot(), promote);
    // Kept populated for the reverse async-upload stamp below (it needs the submission token); cleared there.
    for (auto const& t : cmd->_touched_textures)
        for (auto const& sb : t->finalize_slot(cmd->slot(), promote))
            emit_texture_barrier(cmd->_list.Get(), t->_resource.Get(), sb.range, sb.barrier);
    cmd->_touched_textures.clear();

    HRESULT const hr = cmd->_list->Close();
    CC_ASSERT(SUCCEEDED(hr), "ID3D12GraphicsCommandList::Close failed");

    // Execute, take a monotonic completion token, and signal it — all under one lock so token order
    // equals queue submission and signal order. (The queue is free-threaded, but out-of-order signals
    // would move the fence's completed value backwards and break is_submission_complete.)
    sg::submission_token const token = _next_submission.lock(
        [&](sg::submission_token& next)
        {
            // If this list reads a buffer an async upload is still writing, make the direct queue wait on
            // the copy queue's completion fence before executing, so the copy is visible. Over-waiting on
            // a higher value is safe; a stale/already-signaled value returns immediately.
            if (cmd->_required_copy_wait != dx12_copy_fence_value::none)
                _queue->Wait(_copy_fence.Get(), cc::u64(cmd->_required_copy_wait));

            ID3D12CommandList* lists[] = {cmd->_list.Get()};
            _queue->ExecuteCommandLists(1, lists);

            sg::submission_token const t = next;
            next = sg::submission_token(cc::u64(next) + 1);
            HRESULT const sig = _queue->Signal(_submission_fence.Get(), cc::u64(t));
            CC_ASSERT(SUCCEEDED(sig), "ID3D12CommandQueue::Signal failed");

            // Stamp this list's deferred downloads with the token and hand them to the actor under the
            // same lock, so the actor's copy order matches submission (and thus ring-allocation) order.
            _download_inline.enqueue_submitted(t, cmd->_pending_downloads);
            return t;
        });

    // Stamp every buffer this list touched with the token, so a later async upload to it defers its copy
    // behind this list (the reverse per-resource cross-queue sync). Reuses the access tracker's touched-buffer
    // set. Done after submit returns the token — the caller then issues the async upload, which reads this
    // stamp, so the ordering holds.
    for (auto const& buf : cmd->_touched_buffers)
    {
        cc::u64 prev = buf->_last_used_submission_token.load(std::memory_order_relaxed);
        while (prev < cc::u64(token)
               && !buf->_last_used_submission_token.compare_exchange_weak(
                   prev, cc::u64(token), std::memory_order_release, std::memory_order_relaxed))
        {
            // CAS retries; `prev` is refreshed with the current value each time.
        }
    }
    cmd->_touched_buffers.clear();

    // The allocator is in flight until this epoch retires — hand it to the pool's epoch capture. The
    // list is already closed and can be reused now (resetting an in-flight list onto a fresh, GPU-safe
    // allocator is legal), so return it to the pool for the next acquire.
    _cmd_pool.return_command_list(cmd->_queue, cc::move(cmd->_list));
    _cmd_pool.return_submitted_allocator({cc::move(cmd->_allocator), cmd->_queue});
    (void)_command_list_slots.release(cmd->slot());
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
    cmd->_consumed = true; // its dtor must not auto-drop it
    return token;
}

void dx12_context::drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be dropped in the epoch it was opened "
                                                          "in");
    reclaim_unsubmitted_command_list(*cmd);
}

void dx12_context::reclaim_unsubmitted_command_list(dx12_command_list& cmd)
{
    CC_ASSERT(!cmd._consumed, "command list already submitted or dropped");
    cmd._consumed = true;

    // Never submitted, so its recorded downloads will never run — reclaim their reserved readback space.
    _download_inline.discard_unsubmitted(cmd._pending_downloads);

    // The recorded work never runs, so its declared accesses leave no committed state: just clear each
    // touched buffer's slot (canonical unchanged), then release the slot.
    for (auto const& b : cmd._touched_buffers)
        b->discard_slot(cmd.slot());
    cmd._touched_buffers.clear();
    for (auto const& t : cmd._touched_textures)
        t->discard_slot(cmd.slot());
    cmd._touched_textures.clear();
    (void)_command_list_slots.release(cmd.slot());

    // Never submitted, so the GPU never touched this allocator. Close the list so it is poolable, then
    // return both to the pool: the list for reuse, the allocator straight to the free pool (it was
    // never executed, so no epoch gates it; reset happens at reuse).
    HRESULT const closed = cmd._list->Close();
    CC_ASSERT(SUCCEEDED(closed), "ID3D12GraphicsCommandList::Close failed");
    _cmd_pool.return_command_list(cmd._queue, cc::move(cmd._list));
    _cmd_pool.return_free_allocator({cc::move(cmd._allocator), cmd._queue});
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
}

dx12_command_list::~dx12_command_list()
{
    if (_consumed)
        return; // submitted or dropped explicitly — nothing to reclaim

    // Safety net: a list left neither submitted nor dropped. Reclaim it like a drop so the open-list
    // count, its slot, and its allocator/list don't leak — but warn, since the explicit call is required.
    cc::eprintln("[sg] command list destroyed without submit or drop — auto-dropping. Submit or drop every "
                 "command list explicitly through the context.");
    _ctx.reclaim_unsubmitted_command_list(*this);
}
} // namespace sg::backend::dx12
