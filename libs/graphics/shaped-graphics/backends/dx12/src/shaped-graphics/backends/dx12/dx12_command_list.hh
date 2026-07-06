#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_download_inline.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::command_list. Owns its allocator and graphics command list,
/// handed out already recording. Buffer transfers stage through the context's inline upload/download
/// systems; downloads accumulate here token-less and are enqueued when the list is submitted.
class dx12_command_list final : public sg::command_list
{
public:
    dx12_command_list(dx12_context& ctx,
                      sg::epoch created_in,
                      sg::command_list_slot slot,
                      D3D12_COMMAND_LIST_TYPE queue,
                      ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList> list)
      : sg::command_list(created_in, slot),
        _ctx(ctx),
        _queue(queue),
        _allocator(cc::move(allocator)),
        _list(cc::move(list))
    {
    }

    dx12_context& _ctx;             // creating context — outlives this list
    D3D12_COMMAND_LIST_TYPE _queue; // queue the allocator/list belong to — routes them back to the pool
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;

    // Deferred readback copies recorded into this list; stamped with the submission token and handed
    // to the download system at submit (empty for a list with no downloads).
    cc::vector<dx12_download_copy_job> _pending_downloads;

    // Access tracking: buffers this list has touched (so their slots are finalized at submit/drop, and so
    // each gets the reverse async-upload stamp at submit) and the group currently bound to compute set 0
    // (whose views are declared at dispatch).
    cc::vector<dx12_buffer_handle> _touched_buffers;
    dx12_binding_group const* _bound_group = nullptr;

    // Highest async-upload completion value any buffer this list touches is waiting on. At submit the
    // direct queue waits on the copy fence for this value, so the list sees the async writes. `none`
    // means no touched buffer had a pending async upload. Maintained by track_buffer_access; the reverse
    // stamp (defer a later async upload behind this list) is applied to _touched_buffers at submit.
    dx12_copy_fence_value _required_copy_wait = dx12_copy_fence_value::none;

protected:
    // Reached through the base's cmd.upload / cmd.download / cmd.copy scopes.
    void upload_bytes_to_buffer(sg::buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes) override;

    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::buffer_handle buffer,
                                                              cc::isize offset_in_bytes,
                                                              cc::isize size_in_bytes) override;

    void copy_buffer_region(sg::buffer_handle src,
                            sg::buffer_handle dst,
                            cc::isize src_offset_in_bytes,
                            cc::isize dst_offset_in_bytes,
                            cc::isize size_in_bytes) override;

    // Compute recording (reached through cmd.compute). Bodies in dx12_command_list.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int set, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_declare_array_buffer_access(cc::string_view binding_name,
                                             cc::span<sg::array_buffer_access const> elements) override;
    void compute_declare_array_texture_access(cc::string_view binding_name,
                                              cc::span<sg::array_texture_access const> elements) override;

private:
    // Declare `stages`/`access` on `buffer` for this list's slot, emit the intra-list barrier the tracker
    // asks for (COPY_DEST→COPY_SOURCE and the like — precise, no bounce through COMMON), and record the
    // buffer so its slot is finalized at submit/drop. Cross-list ordering rides on D3D12's decay of buffers
    // to COMMON at ExecuteCommandLists, so no trailing barrier is needed. Also folds the buffer's pending
    // async-upload value into _required_copy_wait (the forward cross-queue sync for ctx.upload).
    void track_buffer_access(dx12_buffer_handle const& buffer, sg::pipeline_stage_flags stages, sg::access_flags access);
};
} // namespace sg::backend::dx12
