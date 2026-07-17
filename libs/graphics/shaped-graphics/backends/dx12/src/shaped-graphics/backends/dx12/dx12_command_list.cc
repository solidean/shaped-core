// dx12_command_list: allocation, submission, and drop. The list type is header-only (ctor +
// fields); its create/submit/drop bodies live here. Allocators are epoch-gated (recycled once the
// epoch retires); see libs/graphics/shaped-graphics/docs/concepts/epochs.md.

#include <clean-core/string/print.hh>
#include <shaped-graphics/backend/access_inference.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_shader_table.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/exceptions.hh>

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

    // Reverse cross-queue sync for ctx.download: if this op WRITES a buffer an async readback is still
    // reading on the copy queue, the direct queue must wait for that read to finish before overwriting it.
    // Only writes conflict (two reads don't), so fold the download value only for a write access.
    if (sg::is_unordered_write(access))
    {
        cc::u64 const dl_v = buffer->_pending_async_download_value.load(std::memory_order_acquire);
        if (dl_v > cc::u64(_required_download_wait))
            _required_download_wait = dx12_download_fence_value(dl_v);
    }

    // Accumulate the access (no barrier yet). Declaring — not flushing — here is what lets a buffer bound
    // several times to one op merge into a single barrier with the union of its stages/access; flush_barriers()
    // does the merge just before the op. mark_pending_barrier enqueues it for that flush exactly once — true
    // only on the first binding this op — so the buffer appears in _pending_barrier_buffers at most once.
    buffer->declare_access(slot(), stages, access);
    if (buffer->mark_pending_barrier(slot()))
        _pending_barrier_buffers.push_back(buffer);

    // Record once so its slot is finalized at submit/drop and it gets the reverse async-upload stamp at
    // submit. The per-slot recorded flag makes the dedup O(1) — no scan of _touched_buffers.
    if (buffer->mark_recorded(slot()))
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

    // Cross-queue forward waits, mirroring track_buffer_access: if a ctx.upload wrote this texture on the
    // copy queue, this list must wait for that copy; and if this access WRITES and a ctx.download is still
    // reading it, wait for the readback first. Both fold into a single per-list wait issued at submit.
    cc::u64 const async_up = texture->_pending_async_upload_value.load(std::memory_order_acquire);
    if (async_up > cc::u64(_required_copy_wait))
        _required_copy_wait = dx12_copy_fence_value(async_up);
    if (sg::is_unordered_write(access))
    {
        cc::u64 const async_dl = texture->_pending_async_download_value.load(std::memory_order_acquire);
        if (async_dl > cc::u64(_required_download_wait))
            _required_download_wait = dx12_download_fence_value(async_dl);
    }

    // Accumulate the access over the range (no barrier yet). Declaring — not flushing — here is what lets a
    // texture bound several times to one op merge its declares into one barrier per subresource box;
    // flush_barriers() does the merge just before the op. mark_pending_barrier enqueues it for that flush
    // exactly once — true only on the first binding this op — so it appears in _pending_barrier_textures once.
    texture->declare_texture_access(slot(), range, stages, access, layout);
    if (texture->mark_pending_barrier(slot()))
        _pending_barrier_textures.push_back(texture);

    // Record once so its slot is finalized at submit/drop. The per-slot recorded flag makes the dedup O(1) —
    // no scan of _touched_textures.
    if (texture->mark_recorded(slot()))
        _touched_textures.push_back(texture);
}

void dx12_command_list::flush_barriers()
{
    // Flush every resource whose access was declared since the last flush. Flushing here (not per declare)
    // merges the several declares of a resource bound more than once to this op into one barrier carrying the
    // union of stages/access. Each resource appears at most once here (track_* enqueues it only on the first
    // binding this op), so each is flushed exactly once.
    for (auto const& b : _pending_barrier_buffers)
        if (auto const bar = b->flush_access(slot()); bar.needed)
            _pending_buffer_barriers.push_back(make_buffer_barrier(b->_resource.Get(), bar));
    for (auto const& t : _pending_barrier_textures)
        for (auto const& sb : t->flush_texture_access(slot()))
            _pending_texture_barriers.push_back(make_texture_barrier(t->_resource.Get(), sb.range, sb.barrier));
    _pending_barrier_buffers.clear();
    _pending_barrier_textures.clear();

    submit_barriers(_list.Get(), _pending_buffer_barriers, _pending_texture_barriers);
    _pending_buffer_barriers.clear();
    _pending_texture_barriers.clear();
}

void dx12_command_list::transition_texture_to(dx12_texture_handle const& texture, sg::texture_layout layout)
{
    CC_ASSERT(texture != nullptr, "transition_texture_to: texture is null");
    // A pure layout transition: no destination stage/access (the consumer is outside this queue's timeline,
    // e.g. Present). track_texture_access reads the tracked source state to build the barrier, so this
    // composes with whatever layout the frame's render pass left the texture in.
    auto const whole = sg::subresource_range::whole(subresource_extent_of(texture->description()));
    track_texture_access(texture, whole, sg::pipeline_stage_flags::none, sg::access_flags::none, layout);
    flush_barriers();
}

void dx12_command_list::compute_bind_pipeline(sg::compute_pipeline const& pipeline)
{
    auto const* dp = dynamic_cast<dx12_compute_pipeline const*>(&pipeline);
    CC_ASSERT(dp != nullptr, "compute_pipeline is not a dx12 compute_pipeline");

    // The shader-visible heaps must be set before any root descriptor table is bound — one CBV/SRV/UAV
    // heap and one SAMPLER heap (D3D12 allows at most one of each bound at a time).
    ID3D12DescriptorHeap* heaps[] = {_ctx._descriptor_heap.heap.Get(), _ctx._sampler_heap.heap.Get()};
    _list->SetDescriptorHeaps(2, heaps);
    _list->SetComputeRootSignature(dp->layout->root_signature.Get());
    _list->SetPipelineState(dp->pipeline_state.Get());

    // The pipeline layout supplies each slot's root-parameter indices for bind_group; reset the per-slot
    // bound groups to one nullptr per group slot (a new pipeline may have a different slot count).
    _bound_pipeline_layout = dp->layout.get();
    _bound_groups.clear_resize_to_filled(_bound_pipeline_layout->groups.size(), nullptr);
}

void dx12_command_list::compute_bind_group(int set, sg::binding_group const& group)
{
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a compute pipeline before binding groups");
    CC_ASSERT(set >= 0 && set < int(_bound_groups.size()), "binding-group slot out of range for the bound pipeline "
                                                           "layout");

    auto const* dg = dynamic_cast<dx12_binding_group const*>(&group);
    CC_ASSERT(dg != nullptr, "binding_group is not a dx12 binding_group");

    // Transient expiry tripwire: a transient group's descriptor slots are recycled after its epoch, so
    // binding one past that epoch would point the table at another epoch's descriptors.
    CC_ASSERT(!(dg->transient && dg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");

    // The group's own schema must match what the pipeline layout declared at this slot (same root-signature
    // table shape), otherwise the descriptor tables below would be bound against the wrong parameters.
    auto const& gslot = _bound_pipeline_layout->groups[set];
    CC_ASSERT(dg->layout == gslot.layout, "binding_group's layout does not match the pipeline layout's slot");

    // Remember the bound group so its views' accesses are declared at dispatch (the point work runs). The
    // forward async-upload wait for each bound buffer is folded in there too, via track_buffer_access. The
    // root-parameter indices come from the pipeline layout's slot, not the group.
    _bound_groups[set] = dg;
    if (gslot.resource_root_param >= 0)
        _list->SetComputeRootDescriptorTable(UINT(gslot.resource_root_param), dg->table_start);
    if (gslot.sampler_root_param >= 0)
        _list->SetComputeRootDescriptorTable(UINT(gslot.sampler_root_param), dg->sampler_table_start);
}

void dx12_command_list::compute_set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset)
{
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a compute pipeline before setting inline constants");
    CC_ASSERT(_bound_pipeline_layout->inline_constants_root_param >= 0, "the bound pipeline layout declares no "
                                                                        "inline_constants block");
    CC_ASSERT(data.size() % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");

    cc::isize const off = offset.value_or(0);
    CC_ASSERT(off >= 0 && off % 4 == 0, "inline-constants offset must be non-negative and a multiple of 4");
    if (offset.has_value())
        CC_ASSERT(off + data.size() <= cc::isize(_bound_pipeline_layout->inline_constants_num_32bit) * 4,
                  "partial inline-constants update exceeds the declared block size");
    else
        CC_ASSERT(data.size() == cc::isize(_bound_pipeline_layout->inline_constants_num_32bit) * 4,
                  "full inline-constants replace must match the declared block size");

    _list->SetComputeRoot32BitConstants(UINT(_bound_pipeline_layout->inline_constants_root_param),
                                        UINT(data.size() / 4), data.data(), UINT(off / 4));
}

void dx12_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");

    // Declare each bound group's shader accesses before the dispatch: the tracker emits any intra-list
    // hazard barrier (e.g. a prior copy_write → shader_read RAW, or a WAW between two dispatches).
    // Cross-list visibility rides on D3D12 decaying buffers to COMMON at ExecuteCommandLists.
    for (auto const* bound_group : _bound_groups)
    {
        if (bound_group == nullptr)
            continue;

        for (auto const& view : bound_group->hazard_views)
            if (view.buffer)
                track_buffer_access(view.buffer, sg::pipeline_stage_flags::compute, sg::shader_access_of(view.access));

        // Bound textures also transition to the layout their access class needs (a sampled texture to
        // shader_readonly, a storage texture to shader_readwrite) — the inferred layout is shader_layout_of.
        for (auto const& tv : bound_group->texture_hazard_views)
            track_texture_access(tv.texture, tv.range, sg::pipeline_stage_flags::compute,
                                 sg::shader_access_of(tv.access), sg::shader_layout_of(tv.access));
    }

    // Emit every hazard the bound resources declared, batched, right before the dispatch consumes them.
    flush_barriers();
    _list->Dispatch(UINT(x), UINT(y), UINT(z));
}

void dx12_command_list::raytracing_bind_pipeline(sg::raytracing_pipeline const& pipeline)
{
    auto const* rp = dynamic_cast<dx12_raytracing_pipeline const*>(&pipeline);
    CC_ASSERT(rp != nullptr, "raytracing_pipeline is not a dx12 raytracing_pipeline");

    // Query the DXR command-list interface OUTSIDE the assert — SetPipelineState1 lives on it, and the
    // As() out-param is a real side effect CC_ASSERT would compile out with asserts off.
    ComPtr<ID3D12GraphicsCommandList4> list4;
    [[maybe_unused]] HRESULT const list4_hr = _list.As(&list4);
    CC_ASSERT(SUCCEEDED(list4_hr) && list4, "ID3D12GraphicsCommandList4 unavailable (SDK/driver too old for DXR)");

    // Ray tracing binds through the compute root signature; set the shader-visible heaps first, then the
    // global root signature, then the state object (SetPipelineState1, not SetPipelineState).
    ID3D12DescriptorHeap* heaps[] = {_ctx._descriptor_heap.heap.Get(), _ctx._sampler_heap.heap.Get()};
    _list->SetDescriptorHeaps(2, heaps);
    _list->SetComputeRootSignature(rp->layout->root_signature.Get());
    list4->SetPipelineState1(rp->state_object.Get());

    _bound_pipeline_layout = rp->layout.get();
    _bound_groups.clear_resize_to_filled(_bound_pipeline_layout->groups.size(), nullptr);
}

void dx12_command_list::raytracing_bind_group(int set, sg::binding_group const& group)
{
    // Identical to compute: DXR binds through the compute root signature.
    compute_bind_group(set, group);
}

void dx12_command_list::raytracing_dispatch_rays(sg::raytracing_shader_table const& table,
                                                 sg::raygen_index raygen,
                                                 int width,
                                                 int height,
                                                 int depth)
{
    CC_ASSERT(width >= 1 && height >= 1 && depth >= 1, "dispatch_rays dimensions must be >= 1");
    CC_ASSERT(cc::i64(width) * cc::i64(height) * cc::i64(depth) <= (cc::i64(1) << 30), "dispatch_rays exceeds the 2^30 "
                                                                                       "total-thread limit");
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a raytracing pipeline before dispatch_rays");

    auto const* dt = dynamic_cast<dx12_raytracing_shader_table const*>(&table);
    CC_ASSERT(dt != nullptr, "raytracing_shader_table is not a dx12 shader table");

    ComPtr<ID3D12GraphicsCommandList4> list4;
    [[maybe_unused]] HRESULT const list4_hr = _list.As(&list4);
    CC_ASSERT(SUCCEEDED(list4_hr) && list4, "ID3D12GraphicsCommandList4 unavailable (SDK/driver too old for DXR)");

    // Declare each bound group's accesses at the raytracing stage (a bound TLAS surfaces as accel_read), same
    // rhythm as compute_dispatch.
    for (auto const* bound_group : _bound_groups)
    {
        if (bound_group == nullptr)
            continue;

        for (auto const& view : bound_group->hazard_views)
            if (view.buffer)
                track_buffer_access(view.buffer, sg::pipeline_stage_flags::raytracing, sg::shader_access_of(view.access));

        for (auto const& tv : bound_group->texture_hazard_views)
            track_texture_access(tv.texture, tv.range, sg::pipeline_stage_flags::raytracing,
                                 sg::shader_access_of(tv.access), sg::shader_layout_of(tv.access));
    }

    // The shader table buffer is read by the fixed-function ray dispatch.
    track_buffer_access(dt->buffer, sg::pipeline_stage_flags::raytracing, sg::access_flags::shader_read);

    flush_barriers();

    D3D12_DISPATCH_RAYS_DESC desc = {};
    desc.RayGenerationShaderRecord = dt->raygen_record(raygen);
    desc.MissShaderTable = dt->miss_table;
    desc.HitGroupTable = dt->hit_table;
    desc.CallableShaderTable = dt->callable_table;
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.Depth = UINT(depth);
    list4->DispatchRays(&desc);
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
    track_buffer_access(dst, sg::pipeline_stage_flags::copy, sg::access_flags::copy_write);
    flush_barriers();
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
        track_buffer_access(src, sg::pipeline_stage_flags::copy, sg::access_flags::copy_read);
    flush_barriers();
    return _ctx._download_inline.download_buffer(*this, *src, offset_in_bytes, size_in_bytes);
}

void dx12_command_list::upload_bytes_to_texture(sg::raw_texture_handle texture,
                                                cc::span<cc::byte const> pixels,
                                                sg::subresource_index const& subresource,
                                                sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "upload target texture is null");
    auto const dst = std::dynamic_pointer_cast<dx12_texture const>(texture);
    CC_ASSERT(dst != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!dst->is_expired(), "upload target is a transient texture used past its epoch (expired)");
    CC_ASSERT(sg::has_flag(dst->usage(), sg::texture_usage::copy_dst), "upload target texture must have "
                                                                       "texture_usage::copy_dst");

    // The region is already resolved (whole subresource / bounds-checked / empty→skipped) by the sg layer.
    dx12_texture_footprint const fp = compute_texture_footprint(dst->description(), subresource, region);
    CC_ASSERT(pixels.size() == fp.tight_size(), "pixel data size does not match the copy region");

    // Transition the target subresource to copy_dst (from whatever it was last used as), then record the copy.
    track_texture_access(dst, subresource, sg::pipeline_stage_flags::copy, sg::access_flags::copy_write,
                         sg::texture_layout::copy_dst);
    flush_barriers();
    _ctx._upload_inline.upload_texture(*this, dst->_resource.Get(), fp, pixels);
}

sg::bytes_future dx12_command_list::download_bytes_from_texture(sg::raw_texture_handle texture,
                                                                sg::subresource_index const& subresource,
                                                                sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "download source texture is null");
    auto const src = std::dynamic_pointer_cast<dx12_texture const>(texture);
    CC_ASSERT(src != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!src->is_expired(), "download source is a transient texture used past its epoch (expired)");
    CC_ASSERT(sg::has_flag(src->usage(), sg::texture_usage::copy_src), "download source texture must have "
                                                                       "texture_usage::copy_src");

    // The region is already resolved (whole subresource / bounds-checked / empty→skipped) by the sg layer.
    dx12_texture_footprint const fp = compute_texture_footprint(src->description(), subresource, region);

    // Transition the source subresource to copy_src, then record the readback copy.
    track_texture_access(src, subresource, sg::pipeline_stage_flags::copy, sg::access_flags::copy_read,
                         sg::texture_layout::copy_src);
    flush_barriers();
    return _ctx._download_inline.download_texture(*this, src->_resource.Get(), fp);
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
        track_buffer_access(s, sg::pipeline_stage_flags::copy,
                            sg::access_flags::copy_read | sg::access_flags::copy_write);
    else
    {
        track_buffer_access(s, sg::pipeline_stage_flags::copy, sg::access_flags::copy_read);
        track_buffer_access(d, sg::pipeline_stage_flags::copy, sg::access_flags::copy_write);
    }

    flush_barriers();
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
    // an access-tracking slot that keys its private per-resource state (released on submit/drop). The slot
    // allocator is internally synchronized; creation touches no resource layout, so it needs no extra sync.
    return std::make_unique<dx12_command_list>(*this, current_epoch(), _command_list_slots.acquire(), queue,
                                               cc::move(acquired.value().allocator.allocator),
                                               cc::move(acquired.value().list));
}

sg::submission_token dx12_context::submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in (it cannot span epochs)");

    // Finalize + Close + Execute + Signal all run under _next_submission, so for each list they are one
    // atomic step in a single global order. finalize writes each touched resource's canonical layout and the
    // ExecuteCommandLists below realizes it, so finalize order must equal execute order — running both under
    // this one lock is what guarantees that (submit is thread-safe / multi_threaded). The revert-vs-promote
    // decision itself is per-resource — the last list to finalize a resource commits its layout — and is made
    // under each resource's own mutex. Work that neither changes a layout nor needs the ordering (the reverse
    // upload/download stamp, pool returns, slot release) runs after the lock.
    sg::submission_token const token = _next_submission.lock(
        [&](sg::submission_token& next) -> sg::submission_token
        {
            // Resolve + read back this list's GPU queries first: it records commands (ResolveQueryData +
            // the readback copy) that must land before Close, and touches a transient resolve buffer whose
            // slot the finalize loop below then commits. Its readbacks ride _pending_downloads and are
            // stamped by enqueue_submitted at the end of this lambda.
            cmd->finalize_queries_before_close();

            // Finalize access tracking before closing. Each resource decides per-itself: the last command
            // list using it commits its final state as the new canonical (the one case that may leave a
            // texture in a new layout); every earlier list rolls back to canonical. For buffers this is a
            // no-op (layout always general); for textures the rollback returns transitions back to the
            // canonical layout, recorded here before Close, plus a hidden-cost warning.
            for (auto const& b : cmd->_touched_buffers)
                b->finalize_slot(cmd->slot());
            // Both sets stay populated for the reverse async-copy stamp below (it needs the submission
            // token); cleared there.
            for (auto const& t : cmd->_touched_textures)
                for (auto const& sb : t->finalize_slot(cmd->slot()))
                    cmd->_pending_texture_barriers.push_back(
                        make_texture_barrier(t->_resource.Get(), sb.range, sb.barrier));
            // Record the finalize reverts (the only barriers left pending — every op flushed its own) before Close.
            cmd->flush_barriers();

            HRESULT const hr = cmd->_list->Close();
            if (FAILED(hr))
            {
                // A lost device surfaces here as a removed HRESULT — throw so the caller tears down and
                // rebuilds (submit is a device-timeline checkpoint). Any other Close failure is an internal
                // bug. Throwing unwinds this lambda and releases _next_submission via its guard.
                if (note_device_removed_if_lost(hr, "command list Close"))
                    throw sg::device_lost_exception(device_loss_reason());
                CC_ASSERT(false, "ID3D12GraphicsCommandList::Close failed");
            }

            // If this list reads a buffer an async upload is still writing, make the direct queue wait on the
            // copy queue's completion fence before executing, so the copy is visible. Over-waiting on a higher
            // value is safe; a stale/already-signaled value returns immediately.
            if (cmd->_required_copy_wait != dx12_copy_fence_value::none)
                _queue->Wait(_upload_async._completion_fence.Get(), cc::u64(cmd->_required_copy_wait));

            // Symmetric reverse sync: if this list WRITES a buffer an async readback is still reading, wait
            // on the download completion fence first, so the write never overwrites bytes the read consumes.
            if (cmd->_required_download_wait != dx12_download_fence_value::none)
                _queue->Wait(_download_async._completion_fence.Get(), cc::u64(cmd->_required_download_wait));

            ID3D12CommandList* lists[] = {cmd->_list.Get()};
            _queue->ExecuteCommandLists(1, lists);

            // Take a monotonic completion token and signal it under this same lock, so token order equals
            // queue submission and signal order. (The queue is free-threaded, but out-of-order signals would
            // move the fence's completed value backwards and break is_submission_complete.)
            sg::submission_token const t = next;
            next = sg::submission_token(cc::u64(next) + 1);
            HRESULT const sig = _queue->Signal(_submission_fence.Get(), cc::u64(t));
            // Record device loss here but don't throw inside the lock; the throw happens after it releases.
            if (FAILED(sig) && !note_device_removed_if_lost(sig, "queue Signal"))
                CC_ASSERT(false, "ID3D12CommandQueue::Signal failed");

            // Stamp this list's deferred downloads with the token and hand them to the actor under the same
            // lock, so the actor's copy order matches submission (and thus ring-allocation) order.
            _download_inline.enqueue_submitted(t, cmd->_pending_downloads);
            return t;
        });

    // The Signal above may have observed device removal (marked, not thrown, inside the lock). Surface it
    // now that the lock is released — the context is dead, so the post-submit bookkeeping is moot.
    if (is_device_lost())
        throw sg::device_lost_exception(device_loss_reason());

    // Stamp every buffer this list touched with the token, so a later async upload to it defers its copy
    // behind this list (the reverse per-resource cross-queue sync). Reuses the access tracker's touched-buffer
    // set. Done after submit returns the token — the caller then issues the async upload, which reads this
    // stamp, so the ordering holds.
    auto const stamp_token = [token](std::atomic<cc::u64>& stamp)
    {
        cc::u64 prev = stamp.load(std::memory_order_relaxed);
        while (prev < cc::u64(token)
               && !stamp.compare_exchange_weak(prev, cc::u64(token), std::memory_order_release, std::memory_order_relaxed))
        {
            // CAS retries; `prev` is refreshed with the current value each time.
        }
    };
    for (auto const& buf : cmd->_touched_buffers)
        stamp_token(buf->_last_used_submission_token);
    for (auto const& tex : cmd->_touched_textures)
        stamp_token(tex->_last_used_submission_token);
    cmd->_touched_buffers.clear();
    cmd->_touched_textures.clear();

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

    // Return any leased query heaps to the pool unresolved. Handles handed out for this list keep an
    // invalid shared future, so they stay valid-but-never-ready (like a cancelled download).
    for (auto& lease : cmd._leased_query_heaps)
        _query_system.release_heap(cc::move(lease));
    cmd._leased_query_heaps.clear();
    cmd._active_timestamp_lease = -1;

    // The recorded work never runs, so its declared accesses leave no canonical state: clear each touched
    // resource's slot — which only decrements that resource's active-slot count (canonical layout
    // unchanged, no barriers) — then release the access-tracking slot. No submit lock: drop changes no
    // layout, and each resource's active-slot count / the slot allocator are already independently synced.
    for (auto const& b : cmd._touched_buffers)
        b->discard_slot(cmd.slot());
    for (auto const& t : cmd._touched_textures)
        t->discard_slot(cmd.slot());
    (void)_command_list_slots.release(cmd.slot());
    cmd._touched_buffers.clear();
    cmd._touched_textures.clear();

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
