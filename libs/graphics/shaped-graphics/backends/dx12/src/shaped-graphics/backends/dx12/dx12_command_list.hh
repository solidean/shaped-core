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
                      D3D12_COMMAND_LIST_TYPE queue,
                      ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList> list)
      : sg::command_list(created_in), _ctx(ctx), _queue(queue), _allocator(cc::move(allocator)), _list(cc::move(list))
    {
    }

    dx12_context& _ctx;             // creating context — outlives this list
    D3D12_COMMAND_LIST_TYPE _queue; // queue the allocator/list belong to — routes them back to the pool
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;

    // Deferred readback copies recorded into this list; stamped with the submission token and handed
    // to the download system at submit (empty for a list with no downloads).
    cc::vector<dx12_download_copy_job> _pending_downloads;

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

    // Compute recording (reached through cmd.compute) — not implemented yet; lands with the dx12
    // compute milestone.
    void compute_bind_pipeline(sg::compute_pipeline const&) override
    {
        CC_UNREACHABLE("dx12 compute bind_pipeline is not implemented yet");
    }
    void compute_bind_group(cc::u32, sg::binding_group const&) override
    {
        CC_UNREACHABLE("dx12 compute bind_group is not implemented yet");
    }
    void compute_dispatch(cc::u32, cc::u32, cc::u32) override
    {
        CC_UNREACHABLE("dx12 compute dispatch is not implemented yet");
    }
};
} // namespace sg::backend::dx12
