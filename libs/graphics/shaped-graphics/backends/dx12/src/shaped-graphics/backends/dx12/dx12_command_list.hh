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
                      ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList> list)
      : sg::command_list(created_in), _ctx(ctx), _allocator(cc::move(allocator)), _list(cc::move(list))
    {
    }

    dx12_context& _ctx; // creating context — outlives this list
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;

    // Deferred readback copies recorded into this list; stamped with the submission token and handed
    // to the download system at submit (empty for a list with no downloads).
    cc::vector<dx12_download_copy_job> _pending_downloads;

protected:
    // Reached through the base's cmd.upload / cmd.download scopes.
    void upload_bytes_to_buffer(sg::buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes) override;

    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::buffer_handle buffer,
                                                              cc::isize offset_in_bytes,
                                                              cc::isize size_in_bytes) override;
};
} // namespace sg::backend::dx12
