#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::min
#include <clean-core/container/span.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
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

/// Texture readback: records CopyTextureRegion from the source subresource's region into the readback
/// buffer (rows padded to 256), plus a deferred CPU copy per chunk that *un-pads* the rows into the
/// tightly-packed host destination. Resumable at row/slice granularity, mirroring dx12_texture_upload:
/// each execute_next_job **self-aligns** its byte window to 512, reads the largest chunk that fits, yields
/// its own deferred copy, and returns bytes consumed *including* the alignment waste (0 if the window can't
/// fit an aligned padded row). A region larger than the window — or one straddling the seam — splits across
/// successive calls. The driver emits the copy_src barrier. `dst` must outlive every deferred copy (kept
/// alive via the future's pin).
struct dx12_texture_download final : dx12_resource_download
{
    dx12_texture_download(ID3D12Resource* src, dx12_texture_footprint const& fp, cc::span<cc::byte> dst)
      : _src(src), _fp(fp), _dst(dst)
    {
    }

    [[nodiscard]] cc::isize total_bytes() const override { return _fp.staged_size(); }
    void prepare(dx12_command_list&) override {}
    [[nodiscard]] bool is_finished() const override { return _rows_done >= total_rows(); }

    /// Staged bytes not yet read (padded). The inline driver reserves this (+ one placement alignment of
    /// slack) so a fits-before-the-seam region lands in a single reservation.
    [[nodiscard]] cc::isize remaining_bytes() const { return (total_rows() - _rows_done) * _fp.padded_pitch; }

    [[nodiscard]] dx12_pending_copy execute_next_job(ID3D12GraphicsCommandList& list,
                                                     dx12_download_allocation const& alloc) override
    {
        CC_ASSERT(!is_finished(), "texture readback already finished");

        // Self-align the window to 512; the waste counts as consumed bytes.
        cc::isize const aligned = align_up(alloc.offset, texture_placement_alignment);
        cc::isize const waste = aligned - alloc.offset;
        if (waste >= alloc.size)
            return {}; // no room past the alignment — caller wraps / rolls the window
        cc::isize const max_rows = (alloc.size - waste) / _fp.padded_pitch;
        if (max_rows == 0)
            return {}; // window can't fit one padded row after alignment

        int const slice = int(_rows_done / _fp.rows);
        int const row = int(_rows_done % _fp.rows);
        dx12_texture_copy_chunk const chunk = next_texture_copy_chunk(_fp, slice, row, max_rows);
        cc::isize const n = chunk.staging_rows();

        bool const whole_slices = chunk.row_start == 0 && chunk.row_count == _fp.rows;
        int const top = chunk.row_start * _fp.block_extent;
        int const bottom
            = whole_slices ? _fp.height : cc::min((chunk.row_start + chunk.row_count) * _fp.block_extent, _fp.height);

        // D3D12 requires a block-compressed source box to have block-aligned (multiple of 4) edges. At a mip
        // edge the extent may be a partial block, so round the box up to the block boundary there (the readback
        // stays tightly packed — only the source box grows to a whole block).
        int box_right = _fp.x + _fp.width;
        int box_bottom = _fp.y + bottom;
        if (_fp.block_extent > 1)
        {
            if (box_right == _fp.sub_width)
                box_right = int(align_up(box_right, _fp.block_extent));
            if (box_bottom == _fp.sub_height)
                box_bottom = int(align_up(box_bottom, _fp.block_extent));
        }

        D3D12_TEXTURE_COPY_LOCATION src_loc = {};
        src_loc.pResource = _src;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = _fp.subresource;

        D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
        dst_loc.pResource = alloc.buffer;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Offset = UINT64(aligned);
        dst_loc.PlacedFootprint.Footprint.Format = _fp.format;
        dst_loc.PlacedFootprint.Footprint.Width = UINT(box_right - _fp.x);
        dst_loc.PlacedFootprint.Footprint.Height = UINT(box_bottom - (_fp.y + top));
        dst_loc.PlacedFootprint.Footprint.Depth = UINT(whole_slices ? chunk.slice_count : 1);
        dst_loc.PlacedFootprint.Footprint.RowPitch = UINT(_fp.padded_pitch);

        D3D12_BOX src_box = {};
        src_box.left = UINT(_fp.x);
        src_box.top = UINT(_fp.y + top);
        src_box.front = UINT(_fp.z + chunk.slice_start);
        src_box.right = UINT(box_right);
        src_box.bottom = UINT(box_bottom);
        src_box.back = UINT(_fp.z + chunk.slice_start + (whole_slices ? chunk.slice_count : 1));
        list.CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

        cc::byte const* const src_base = alloc.base + aligned;
        cc::byte* const dst_base = _dst.data();
        cc::isize const first_row = _rows_done;
        cc::isize const row_bytes = _fp.row_bytes;
        cc::isize const padded = _fp.padded_pitch;
        _rows_done += n;
        return dx12_pending_copy{.deferred_cpu_copy =
                                     [src_base, dst_base, first_row, n, row_bytes, padded]
                                 {
                                     for (cc::isize i = 0; i < n; ++i)
                                         std::memcpy(dst_base + (first_row + i) * row_bytes, src_base + i * padded,
                                                     std::size_t(row_bytes));
                                 },
                                 .bytes = waste + n * padded};
    }

private:
    [[nodiscard]] cc::isize total_rows() const { return cc::isize(_fp.rows) * cc::isize(_fp.depth_slices); }
    [[nodiscard]] static cc::isize align_up(cc::isize v, cc::isize a) { return (v + a - 1) / a * a; }

    ID3D12Resource* _src = nullptr;
    dx12_texture_footprint _fp;
    cc::span<cc::byte> _dst;
    cc::isize _rows_done = 0; // padded staging rows read so far (flat over slices)
};
} // namespace sg::backend::dx12
