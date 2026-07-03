#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

#include <cstring>

namespace sg::backend::dx12
{
/// A window inside the persistently-mapped UPLOAD ring buffer, handed to execute_next_job. `base`
/// points at byte 0 of the mapped buffer; the writable window is [base + offset, base + offset + size),
/// and `offset` is also the GPU-side source offset for the copy. Non-owning.
struct dx12_upload_allocation
{
    ID3D12Resource* buffer = nullptr;
    cc::byte* base = nullptr;
    cc::isize offset = 0;
    cc::isize size = 0;
};

/// Records the copy commands that stage one resource's bytes through the inline UPLOAD ring buffer,
/// hiding whether the destination is a buffer or a texture. The upload system drives it: reserve a
/// window, prepare(), then execute_next_job() (once for a buffer; repeatedly for a chunked texture).
struct dx12_resource_upload
{
    virtual ~dx12_resource_upload() = default;

    /// Total ring-buffer bytes this upload needs.
    [[nodiscard]] virtual cc::isize total_bytes() const = 0;

    /// Records any state transition the destination needs before the copy.
    virtual void prepare(dx12_command_list& cmd) = 0;

    /// Whether all copy commands have been recorded.
    [[nodiscard]] virtual bool is_finished() const = 0;

    /// Copies the next chunk into the allocation window and records the GPU copy. Returns bytes
    /// consumed from the window (0 if it is too small to make progress).
    [[nodiscard]] virtual cc::isize execute_next_job(ID3D12GraphicsCommandList& list, dx12_upload_allocation const& alloc)
        = 0;
};

/// Buffer upload: a single CopyBufferRegion into `dst` at `dst_offset`. The source bytes are read
/// during execute_next_job, so they need only outlive that call.
struct dx12_buffer_upload final : dx12_resource_upload
{
    dx12_buffer_upload(dx12_buffer const& dst, cc::isize dst_offset, cc::span<cc::byte const> data)
      : _dst(dst._resource.Get()), _dst_offset(dst_offset), _data(data)
    {
    }

    [[nodiscard]] cc::isize total_bytes() const override { return _data.size(); }

    // Buffers rely on D3D12 implicit state promotion/decay for copies, so no barrier is needed.
    // TODO: global-barrier placeholder — a real per-resource state-tracking barrier system lands later.
    void prepare(dx12_command_list&) override {}

    [[nodiscard]] bool is_finished() const override { return _done; }

    [[nodiscard]] cc::isize execute_next_job(ID3D12GraphicsCommandList& list, dx12_upload_allocation const& alloc) override
    {
        CC_ASSERT(alloc.size >= _data.size(), "upload allocation smaller than the buffer upload");
        std::memcpy(alloc.base + alloc.offset, _data.data(), std::size_t(_data.size()));
        list.CopyBufferRegion(_dst, UINT64(_dst_offset), alloc.buffer, UINT64(alloc.offset), UINT64(_data.size()));
        _done = true;
        return _data.size();
    }

private:
    ID3D12Resource* _dst = nullptr;
    cc::isize _dst_offset = 0;
    cc::span<cc::byte const> _data;
    bool _done = false;
};

// TODO: dx12_texture_upload — inline texture upload (row-padded CopyTextureRegion, chunked across
// several execute_next_job calls) once textures land.
} // namespace sg::backend::dx12
