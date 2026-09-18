#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>

/// One staged upload: where its bytes sit in the ring, or in a buffer of their own when the ring could not hold them.
struct sg::backend::webgpu::webgpu_upload_span
{
    WGPUBuffer buffer = nullptr;
    isize offset = 0;

    /// Set only for an upload the ring could not hold, and then it owns that one-off buffer.
    wgpu_buffer overflow;
};

/// The staging ring behind `cmd.upload`.
///
/// **Unmapped**, which is what makes it work on a browser: WebGPU maps asynchronously, so a mapped ring would have to wait for its own map before the next list could record into it.
/// Instead the bytes go in with `queue.writeBuffer` at record time, and the command list records a copy out of the ring.
/// A write lands in queue order, ahead of the submit that carries the copy, so the copy reads exactly what was staged.
///
/// A span is reusable once no list that could still copy from it is open.
/// A write made after a list's submit lands after that list's copy in queue order, so a submitted list no longer holds its spans.
/// The ring therefore rewinds to its start whenever the last list holding a span submits or drops, and needs no epoch at all.
///
/// An upload the ring cannot hold — larger than the ring, or overflowing it while another open list holds spans — gets a buffer of its own, with a warning once per epoch.
class sg::backend::webgpu::webgpu_upload_ring
{
public:
    /// Allocates the ring; `capacity_in_bytes` must be > 0.
    void initialize(webgpu_context& ctx, isize capacity_in_bytes);

    /// Drops the ring buffer; safe to call twice.
    void shutdown();

    /// Stages `data` for a copy recorded by a list, padded with zeros to `staged_size`, which must be a whole word and at least `data.size()`.
    /// The span starts at a multiple of `alignment`, a power of two no smaller than a word.
    /// The caller must hold the ring (see acquire_holder) for as long as the copy may still be submitted.
    [[nodiscard]] webgpu_upload_span stage(cc::span<byte const> data,
                                           isize staged_size,
                                           isize alignment = buffer_word_bytes);

    /// Stages `data` with each of its rows moved to a multiple of 256 bytes, as a buffer-to-texture copy places them.
    /// The span starts at a multiple of `block_bytes` as well, which a buffer-to-texture copy requires of its offset.
    [[nodiscard]] webgpu_upload_span stage_rows(cc::span<byte const> data,
                                                isize row_bytes,
                                                isize padded_row,
                                                isize staged_size,
                                                isize block_bytes);

    /// Counts one open list holding spans; paired with release_holder when that list submits or drops.
    void acquire_holder() { ++_holders; }

    /// The last holder going rewinds the ring, and applies a pending budget.
    void release_holder();

    /// Records a new capacity, applied the next time no list holds a span.
    void set_budget(isize bytes);

private:
    /// Space for `size` bytes at a multiple of `alignment`, or nullopt when the ring cannot hold them now.
    [[nodiscard]] cc::optional<isize> reserve(isize size, isize alignment);

    /// A dedicated staging buffer for one upload, with the warning.
    [[nodiscard]] webgpu_upload_span stage_outside_ring(cc::span<byte const> padded);

    void write(WGPUBuffer buffer, isize offset, cc::span<byte const> padded);

    webgpu_context* _ctx = nullptr;
    wgpu_buffer _buffer;
    isize _capacity = 0;
    isize _head = 0;
    int _holders = 0;
    isize _pending_capacity = 0;

    // Reused for padding, since a write copies its bytes before it returns.
    cc::vector<byte> _scratch;

    u64 _last_warned_epoch = 0;
};
