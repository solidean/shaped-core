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

    // Compute recording (reached through cmd.compute). Bodies in dx12_command_list.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int set, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;

private:
    // After a transfer op, returns `buffer` from its just-used copy state to COMMON, so the next op in
    // the *same* list re-promotes it from COMMON — matching the cross-list decay the backend otherwise
    // relies on. Buffers in COMMON are implicitly promoted on use, so no explicit "before" transition is
    // needed; this reset is both the state fix and the write→read ordering point within one list.
    // TODO: replace with tracked per-resource transitions once the state-tracking barrier system lands
    // (it will emit precise, minimal COPY_SOURCE/COPY_DEST transitions instead of bouncing through COMMON).
    void restore_buffer_to_common(dx12_buffer const& buffer, D3D12_RESOURCE_STATES from_state);
};
} // namespace sg::backend::dx12
