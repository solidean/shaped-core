#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::min
#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
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

/// Buffer upload: copies `data` into `dst` at `dst_offset` via CopyBufferRegion. Resumable — each
/// execute_next_job stages as much as the window holds and records that chunk, so an upload larger than
/// the staging window is split across successive calls (a buffer fits one job when the window is big
/// enough; the async copy queue chunks large ones). The source bytes are read during execute_next_job,
/// so they need only outlive the calls that consume them.
struct dx12_buffer_upload final : dx12_resource_upload
{
    dx12_buffer_upload(dx12_buffer const& dst, cc::isize dst_offset, cc::span<cc::byte const> data)
      : dx12_buffer_upload(dst._resource.Get(), dst_offset, data)
    {
    }

    // Raw-resource overload: the async path holds only the ID3D12Resource* (kept alive by the job's
    // buffer handle), not a dx12_buffer reference.
    dx12_buffer_upload(ID3D12Resource* dst, cc::isize dst_offset, cc::span<cc::byte const> data)
      : _dst(dst), _dst_offset(dst_offset), _data(data)
    {
    }

    [[nodiscard]] cc::isize total_bytes() const override { return _data.size(); }

    /// Bytes staged and recorded so far.
    [[nodiscard]] cc::isize consumed() const { return _consumed; }

    // Buffers rely on D3D12 implicit state promotion/decay for copies, so no barrier is needed.
    // TODO: global-barrier placeholder — a real per-resource state-tracking barrier system lands later.
    void prepare(dx12_command_list&) override {}

    [[nodiscard]] bool is_finished() const override { return _consumed == _data.size(); }

    [[nodiscard]] cc::isize execute_next_job(ID3D12GraphicsCommandList& list, dx12_upload_allocation const& alloc) override
    {
        cc::isize const remaining = _data.size() - _consumed;
        cc::isize const n = remaining < alloc.size ? remaining : alloc.size;
        CC_ASSERT(n > 0, "upload allocation too small to make progress");
        std::memcpy(alloc.base + alloc.offset, _data.data() + _consumed, std::size_t(n));
        list.CopyBufferRegion(_dst, UINT64(_dst_offset + _consumed), alloc.buffer, UINT64(alloc.offset), UINT64(n));
        _consumed += n;
        return n;
    }

private:
    ID3D12Resource* _dst = nullptr;
    cc::isize _dst_offset = 0;
    cc::span<cc::byte const> _data;
    cc::isize _consumed = 0;
};

/// Texture upload: stages the region's rows — each padded to the D3D12 row-pitch alignment (256) — into
/// the staging buffer and records CopyTextureRegion. Resumable at row/slice granularity: each
/// execute_next_job is handed a plain byte window, **self-aligns** it to the 512-byte placement alignment,
/// stages the largest chunk that fits (a run of whole depth slices, or a partial block-row run within one
/// slice; see next_texture_copy_chunk), and returns the bytes consumed *including* the alignment waste — so
/// the ring/window stays byte-agnostic. A region larger than the free window, or one straddling the seam,
/// splits across successive calls; a window too small to fit an aligned padded row returns 0 (the driver
/// then wraps / rolls to a fresh window). Only a single padded row wider than a whole ring/window is
/// unsupported (a very wide 1D texture). Layout barriers are the driver's job, so `prepare` is a no-op.
struct dx12_texture_upload final : dx12_resource_upload
{
    dx12_texture_upload(ID3D12Resource* dst, dx12_texture_footprint const& fp, cc::span<cc::byte const> data)
      : _dst(dst), _fp(fp), _data(data)
    {
    }

    [[nodiscard]] cc::isize total_bytes() const override { return _fp.staged_size(); }
    void prepare(dx12_command_list&) override {}
    [[nodiscard]] bool is_finished() const override { return _rows_done >= total_rows(); }

    /// Staged bytes not yet recorded (padded). The inline driver reserves this (+ one placement alignment of
    /// slack for the self-alignment) so a fits-before-the-seam region lands in a single reservation.
    [[nodiscard]] cc::isize remaining_bytes() const { return (total_rows() - _rows_done) * _fp.padded_pitch; }

    [[nodiscard]] cc::isize execute_next_job(ID3D12GraphicsCommandList& list, dx12_upload_allocation const& alloc) override
    {
        CC_ASSERT(!is_finished(), "texture upload already finished");

        // Self-align the window to the 512-byte placement alignment; the waste counts as consumed bytes.
        cc::isize const aligned = align_up(alloc.offset, texture_placement_alignment);
        cc::isize const waste = aligned - alloc.offset;
        if (waste >= alloc.size)
            return 0; // no room past the alignment — caller wraps / rolls the window
        cc::isize const max_rows = (alloc.size - waste) / _fp.padded_pitch;
        if (max_rows == 0)
            return 0; // window can't fit one padded row after alignment

        int const slice = int(_rows_done / _fp.rows);
        int const row = int(_rows_done % _fp.rows);
        dx12_texture_copy_chunk const chunk = next_texture_copy_chunk(_fp, slice, row, max_rows);
        cc::isize const n = chunk.staging_rows();

        // Copy each tightly-packed source row into the padded staging layout (rows are contiguous across the
        // chunk's slices in both the source and the staging buffer, so one flat loop covers both shapes).
        cc::byte* const base = alloc.base + aligned;
        for (cc::isize i = 0; i < n; ++i)
            std::memcpy(base + i * _fp.padded_pitch, _data.data() + (_rows_done + i) * _fp.row_bytes,
                        std::size_t(_fp.row_bytes));

        D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
        dst_loc.pResource = _dst;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = _fp.subresource;

        bool const whole_slices = chunk.row_start == 0 && chunk.row_count == _fp.rows;
        int const top = chunk.row_start * _fp.block_extent;
        int const bottom
            = whole_slices ? _fp.height : cc::min((chunk.row_start + chunk.row_count) * _fp.block_extent, _fp.height);

        D3D12_TEXTURE_COPY_LOCATION src_loc = {};
        src_loc.pResource = alloc.buffer;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset = UINT64(aligned);
        src_loc.PlacedFootprint.Footprint.Format = _fp.format;
        src_loc.PlacedFootprint.Footprint.Width = UINT(_fp.width);
        src_loc.PlacedFootprint.Footprint.Height = UINT(whole_slices ? _fp.height : bottom - top);
        src_loc.PlacedFootprint.Footprint.Depth = UINT(whole_slices ? chunk.slice_count : 1);
        src_loc.PlacedFootprint.Footprint.RowPitch = UINT(_fp.padded_pitch);

        list.CopyTextureRegion(&dst_loc, UINT(_fp.x), UINT(_fp.y + top), UINT(_fp.z + chunk.slice_start), &src_loc,
                               nullptr);
        _rows_done += n;
        return waste + n * _fp.padded_pitch;
    }

private:
    [[nodiscard]] cc::isize total_rows() const { return cc::isize(_fp.rows) * cc::isize(_fp.depth_slices); }
    [[nodiscard]] static cc::isize align_up(cc::isize v, cc::isize a) { return (v + a - 1) / a * a; }

    ID3D12Resource* _dst = nullptr;
    dx12_texture_footprint _fp;
    cc::span<cc::byte const> _data;
    cc::isize _rows_done = 0; // padded staging rows recorded so far (flat over slices)
};
} // namespace sg::backend::dx12
