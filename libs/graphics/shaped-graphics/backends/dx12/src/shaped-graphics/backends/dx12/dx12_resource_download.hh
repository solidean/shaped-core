#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

#include <cstring>

namespace sg::backend::dx12
{
/// A window inside the persistently-mapped READBACK ring buffer, handed to execute_next_job. The GPU
/// copies into [offset, offset + size); the deferred CPU copy later reads from base + offset. `base`
/// points at byte 0 of the mapped buffer. Non-owning.
struct dx12_download_allocation
{
    ID3D12Resource* buffer = nullptr;
    cc::byte const* base = nullptr;
    cc::isize offset = 0;
    cc::isize size = 0;
};

/// The CPU-side move of one chunk out of the readback buffer, deferred until the GPU copy completes.
struct dx12_pending_copy
{
    cc::unique_function<void()> deferred_cpu_copy;
    cc::isize bytes = 0;
};

/// Records the readback copy commands for one resource through the inline READBACK ring buffer,
/// hiding buffer vs texture. Mirrors dx12_resource_upload; because readback is asynchronous, each job
/// also yields a deferred CPU copy to run once the GPU work has finished.
struct dx12_resource_download
{
    virtual ~dx12_resource_download() = default;

    /// Total ring-buffer bytes this download needs.
    [[nodiscard]] virtual cc::isize total_bytes() const = 0;

    /// Records any state transition the source needs before the copy.
    virtual void prepare(dx12_command_list& cmd) = 0;

    /// Whether all copy commands have been recorded.
    [[nodiscard]] virtual bool is_finished() const = 0;

    /// Records the next readback copy and returns the deferred CPU copy to run after GPU completion.
    /// A zero-byte result means the allocation was too small to make progress.
    [[nodiscard]] virtual dx12_pending_copy execute_next_job(ID3D12GraphicsCommandList& list,
                                                             dx12_download_allocation const& alloc)
        = 0;
};

/// Buffer download: CopyBufferRegion from `src` into the readback window, then a deferred memcpy into
/// `dst`. Resumable — each execute_next_job reads as much as the window holds and yields that chunk's
/// deferred copy, so a download larger than the readback window is split across successive calls (it fits
/// one job when the window is big enough; the async copy queue chunks large ones). `dst` must outlive
/// every deferred copy (kept alive via the future's pin).
struct dx12_buffer_download final : dx12_resource_download
{
    dx12_buffer_download(dx12_buffer const& src, cc::isize src_offset, cc::span<cc::byte> dst)
      : dx12_buffer_download(src._resource.Get(), src_offset, dst)
    {
    }

    // Raw-resource overload: the async path holds only the ID3D12Resource* (kept alive by the job's
    // buffer handle), not a dx12_buffer reference.
    dx12_buffer_download(ID3D12Resource* src, cc::isize src_offset, cc::span<cc::byte> dst)
      : _src(src), _src_offset(src_offset), _dst(dst)
    {
    }

    [[nodiscard]] cc::isize total_bytes() const override { return _dst.size(); }

    /// Bytes read and recorded so far.
    [[nodiscard]] cc::isize consumed() const { return _consumed; }

    // See dx12_buffer_upload::prepare — buffers need no explicit barrier for copies.
    void prepare(dx12_command_list&) override {}

    [[nodiscard]] bool is_finished() const override { return _consumed == _dst.size(); }

    [[nodiscard]] dx12_pending_copy execute_next_job(ID3D12GraphicsCommandList& list,
                                                     dx12_download_allocation const& alloc) override
    {
        cc::isize const remaining = _dst.size() - _consumed;
        cc::isize const n = remaining < alloc.size ? remaining : alloc.size;
        CC_ASSERT(n > 0, "readback allocation too small to make progress");
        list.CopyBufferRegion(alloc.buffer, UINT64(alloc.offset), _src, UINT64(_src_offset + _consumed), UINT64(n));

        cc::byte const* const src_ptr = alloc.base + alloc.offset;
        cc::byte* const dst_ptr = _dst.data() + _consumed;
        auto const size = std::size_t(n);
        _consumed += n;
        return dx12_pending_copy{[dst_ptr, src_ptr, size] { std::memcpy(dst_ptr, src_ptr, size); }, n};
    }

private:
    ID3D12Resource* _src = nullptr;
    cc::isize _src_offset = 0;
    cc::span<cc::byte> _dst;
    cc::isize _consumed = 0;
};

// TODO: dx12_texture_download — inline texture readback (row-unpadding in the deferred copy, chunked
// across several execute_next_job calls) once textures land.
} // namespace sg::backend::dx12
